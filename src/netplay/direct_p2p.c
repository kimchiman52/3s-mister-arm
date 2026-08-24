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
     * the v1 room code shrank to 11 chars (see room_code.h; the code is
     * 18 chars as of v3 but has never carried local_port again). Hairpin
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

    /* S4a punch-auth token (Rendezvous_DerivePunchToken over the same
     * payload the room code encodes). HOST: derived by host_thread_fn
     * from its advertised tuple right after the room code is built
     * (re-derived by the drift handler when the code is re-encoded);
     * consumed by host_tick_receive's datagram gate and the bilateral
     * punch worker. JOINER: derived per join_attempt from the decoded
     * code into a worker-stack copy — this field stays host-only.
     * token_valid gates the host's punch gate fail-closed: with no
     * valid token NO datagram is ever accepted as the peer. */
    uint8_t punch_token[STUN_PUNCH_TOKEN_LEN];
    bool punch_token_valid;

    /* S4b room-code nonce (32 bits as of v3). HOST: drawn from the CSPRNG by
     * host_thread_fn right before the room code is encoded; kept
     * STABLE across a drift re-encode (the endpoint change already
     * forces a new code/key/token — regenerating the nonce too would
     * add nothing) and regenerated per hosting attempt. JOINER: decoded
     * from the room code by BeginJoin. Feeds the session-key and
     * punch-token derivations on both roles. */
    uint32_t nonce;

    /* Parsed rendezvous endpoint cache; populated in BeginHost/BeginJoin
     * once 5b/5c land. Holds the result of parsing the signal-url config
     * value (host:port). Both fields zero in 5a — no callers yet. */
    char     signal_host[64];
    uint16_t signal_port;

    /* --- S3 failure attribution (docs/plan-netplay-connection.md §5) ---
     * Written by the worker BEFORE it publishes a terminal state (same
     * discipline as every other s_work field); read by the main thread's
     * Tick reporting path after it observes that state. */
    ConnectFailCode fail_code;   /* taxonomy code for the surfaced failure */
    bool ev_deliver_any;         /* joiner: >=1 DELIVER frame (incl. sentinel) */
    bool ev_deliver_real;        /* joiner: >=1 DELIVER with a real endpoint */
    bool ev_challenge_any;       /* S4c joiner: >=1 CHALLENGE frame received */
    int  join_attempts;          /* attempts consumed (S2 auto-retry) */
    /* Stage timings (ms) for the report line — 0 = stage not reached. */
    uint32_t t_upnp_ms;
    uint32_t t_stun_ms;
    uint32_t t_punch_ms;
    uint32_t t_signal_ms;
    uint32_t t_bilateral_ms;
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

/* Review M2: drift debounce. A single differing STUN Binding Response
 * must not rewrite the on-screen room code — the user may be reading it
 * aloud. A drift candidate is only COMMITTED when a second consecutive
 * keepalive confirms the same new endpoint; a response matching the
 * current endpoint clears the candidate. Deliberate side effect: a NAT
 * that rebinds on every keepalive (UDP idle timeout shorter than the
 * keepalive interval) produces a DIFFERENT port each time, so the
 * candidate keeps being replaced and never commits — the code stays
 * stable instead of churning every interval with "Network changed!".
 * Main-thread only, same lifecycle as the txid state above. */
static char s_drift_pending_ip[64] = { 0 };
static uint16_t s_drift_pending_port = 0;
static bool s_drift_pending_valid = false;

/* S2 host auto-retry (docs/plan-netplay-connection.md §4): a host
 * parking terminal in FAILED_STUN because one discovery attempt raced
 * a DHCP renew / DNS hiccup is pure loss — the user just stares at
 * ERROR. Tick's FAILED_STUN case re-spawns host_thread_fn after a 5 s
 * backoff, up to HOST_STUN_MAX_RETRIES per hosting session (BeginHost
 * resets the count). Main-thread only, like the other Tick-side
 * bookkeeping. The JOINER's FAILED_STUN is handled inside its own
 * worker (join_thread_fn single fresh-socket retry) and stays terminal
 * here. */
static int s_host_stun_retry_count = 0;
static uint64_t s_host_stun_retry_at_ms = 0;
#define HOST_STUN_MAX_RETRIES 3
#define HOST_STUN_RETRY_BACKOFF_MS 5000

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

/* --- S3: failure attribution helpers ----------------------------------- */

/* One attributed report line per Begin* attempt-set (reset alongside the
 * other per-session bookkeeping). Main-thread only — written from Tick's
 * reporting path and the BeginHost / BeginJoin / Cancel resets. */
static bool s_outcome_reported = false;

/* S3: has the host received ANY rendezvous DELIVER (incl. the
 * zero-sentinel) this hosting session? The server answers every REGISTER
 * with a DELIVER, so this doubles as "the rendezvous path is alive" for
 * the cause-8 host advisory. Main-thread only (try_handle_deliver runs
 * from Tick). */
static bool s_host_deliver_seen = false;

/* S3: set by host_rendezvous_thread_fn when it actually ENTERS its
 * REGISTER resend loop. The cause-8 advisory reads "zero DELIVERs" as
 * evidence of a dead rendezvous path — which is only evidence when
 * REGISTERs are in fact being sent. With the bilateral kill switch on,
 * a missing/malformed signal URL, a failed resolve, or a failed thread
 * spawn, no REGISTER ever leaves the box and silence is EXPECTED, so
 * the advisory must stay quiet (those paths already log their own
 * cause). Worker-write / main-read. */
static SDL_AtomicInt s_host_registering = { 0 };

/* S3 Part A(3): HOST_WAITING is legitimately unbounded by design — a
 * host advertises until a joiner arrives or the user cancels. The S3
 * requirement is that it be INFORMATIVE, not silent: elapsed time on the
 * status line (so the user can see the room is alive), a minute-cadence
 * log line, and the cause-8 advisory when the evidence says the room is
 * likely unjoinable. Main-thread only (Tick's HOST_WAITING branch).
 * NOT reset on the M1 bilateral-failure return to HOST_WAITING — that is
 * the same hosting session. */
static uint64_t s_host_waiting_since_ms = 0;
#ifdef NETPLAY_TEST_HOOKS
/* Multiplier applied to the host-waiting elapsed clock (test seam; 1 =
 * off). See host_waiting_tick. */
static int s_host_advisory_scale = 1;
#endif
static uint64_t s_host_waiting_last_note_ms = 0;
static ConnectFailCode s_host_advisory_code = CONNECT_FAIL_NONE;

/* S4a: count of unauthenticated datagrams the host's gate has ignored
 * this hosting session (rate-limited advisory logging in
 * host_tick_receive). Main-thread only. */
static int s_host_unauth_drops = 0;

/* --- S4-review HIGH-1b: host punch-gate throttle ----------------------
 *
 * s_host_unauth_drops above is a LOG COUNTER ONLY. Before this block
 * there was no cap, no per-source throttle and no backoff on the host's
 * punch gate: the host answered every guess (a correct token is
 * accepted and handed off, a wrong one is silently dropped and the host
 * keeps waiting — deliberately with NO wall-clock budget), which makes
 * it a perfect brute-force oracle. host_tick_receive drains ~60
 * datagrams/second, and with a UPnP mapping the advertised port is
 * STABLE, so a past opponent could grind at it months later.
 *
 * The v3 room code widened the nonce to 32 bits, which alone takes a
 * 60/s grind from <=68 seconds to >=2.2 years. This throttle is the
 * belt to that suspenders, and it closes the asymmetry the reviewer
 * flagged: the session-KEY path was already properly bounded (no oracle
 * + MAX_NEW_KEYS_PER_IP) while the punch path was not bounded at all.
 *
 * Accounting rule — ONLY punch-SHAPED datagrams that FAIL the token
 * check are charged. A legitimate joiner holding the right code is
 * accepted on its first punch and is never charged at all, so the
 * thresholds below cannot be reached by a peer that can actually
 * connect. What CAN reach them: a scanner, a brute-forcer, or a peer
 * punching with a stale code (pre-drift-re-encode, or an old build).
 *
 * Two levels:
 *
 *  (1) PER SOURCE IP — after HOST_PUNCH_SRC_MAX_BAD bad punches from one
 *      IP that source is MUTED: its punches are dropped WITHOUT the
 *      accept/echo/handoff, so the oracle is closed for it. Sizing: a
 *      joiner's Stun_HolePunch emits ~10 datagrams in its first 500 ms
 *      then 5/s, so 24 covers the opening burst plus ~2.8 s of steady
 *      state before muting — a peer that merely raced a drift re-encode
 *      gets plenty of room.
 *
 *  (2) PER SESSION — after HOST_PUNCH_TOTAL_REROLL bad punches in total
 *      the host re-rolls the nonce, re-encodes the room code, re-derives
 *      the token and TELLS THE USER on the status line. Whatever the
 *      attacker was searching is now worthless. Capped at
 *      HOST_PUNCH_REROLL_MAX per hosting session so a persistently
 *      stale peer cannot churn the displayed code forever.
 *
 * DELIBERATE DEPARTURE from the review's literal "stop classifying that
 * source for the session": the mute EXPIRES after HOST_PUNCH_MUTE_MS,
 * and a re-roll clears every mute. A permanent mute would turn this
 * defence into a self-inflicted lockout — the exact failure class the
 * review's headline question tested S4a for. Concretely: friend types a
 * typo'd code, floods 24 bad punches, gets muted; user reads the code
 * out again; friend now punches with the RIGHT token and is still
 * dropped, forever, with no diagnosis. A bounded mute is also
 * arithmetically sufficient: 24 attempts per 60 s is 0.4/s versus the
 * unthrottled 60/s, so a 32-bit nonce needs ~340 years per source IP,
 * and a 10,000-node botnet still needs ~12 days against a code that
 * only exists while the OSD screen is up.
 *
 * All state here is MAIN-THREAD ONLY (host_tick_receive and the Tick
 * reset paths). No locking.
 */
#define HOST_PUNCH_SRC_TABLE      8      /* tracked source IPs           */
#define HOST_PUNCH_SRC_MAX_BAD    24     /* bad punches before mute      */
#define HOST_PUNCH_MUTE_MS        60000u /* mute lifetime                */
#define HOST_PUNCH_TOTAL_REROLL   64     /* session bad punches -> re-roll */
#define HOST_PUNCH_REROLL_MAX     3      /* re-rolls per hosting session */

typedef struct {
    char     ip[64];       /* "" = free slot                            */
    int      bad;          /* bad punch-shaped datagrams charged        */
    uint32_t mute_until;   /* SDL_GetTicks() deadline; 0 = not muted    */
} HostPunchSrc;

static HostPunchSrc s_host_punch_src[HOST_PUNCH_SRC_TABLE];
static int  s_host_punch_bad_total = 0;   /* this hosting session        */
static int  s_host_punch_rerolls   = 0;   /* code re-rolls performed     */
static int  s_host_punch_muted_drops = 0; /* accepts suppressed by mute  */

static void host_punch_gate_reset(void) {
    memset(s_host_punch_src, 0, sizeof(s_host_punch_src));
    s_host_punch_bad_total = 0;
    s_host_punch_rerolls = 0;
    s_host_punch_muted_drops = 0;
}

/* Clear every mute and every per-source tally, keeping the session
 * totals. Called after a re-roll: the code the muted sources were
 * failing against no longer exists, so holding their strikes against
 * them would be exactly the lockout this design avoids. */
static void host_punch_gate_clear_mutes(void) {
    memset(s_host_punch_src, 0, sizeof(s_host_punch_src));
}

/* Find `ip`'s slot, or claim one. Eviction picks the entry with the
 * FEWEST strikes that is not currently muted — a muted attacker cannot
 * push itself out of the table by rotating source addresses, while a
 * one-off stray packet ages out immediately. Returns NULL only when
 * every slot is muted (in which case the caller just charges the
 * session total, which is the level that actually matters). */
static HostPunchSrc* host_punch_src_slot(const char* ip, uint32_t now_ms) {
    HostPunchSrc* victim = NULL;
    for (int i = 0; i < HOST_PUNCH_SRC_TABLE; i++) {
        HostPunchSrc* e = &s_host_punch_src[i];
        if (e->ip[0] != '\0' && strcmp(e->ip, ip) == 0) {
            /* Expire a stale mute in place. */
            if (e->mute_until != 0 &&
                (int32_t)(now_ms - e->mute_until) >= 0) {
                e->mute_until = 0;
                e->bad = 0;
            }
            return e;
        }
        if (e->ip[0] == '\0') {
            victim = e;
            break;
        }
        const bool muted = e->mute_until != 0 &&
                           (int32_t)(now_ms - e->mute_until) < 0;
        if (muted) continue;
        if (victim == NULL || e->bad < victim->bad) victim = e;
    }
    if (victim == NULL) return NULL;
    SDL_strlcpy(victim->ip, ip, sizeof(victim->ip));
    victim->bad = 0;
    victim->mute_until = 0;
    return victim;
}

/* Is `ip` currently muted? Cheap read-only probe used on the ACCEPT
 * path — the token compare still runs, we simply refuse to answer, so
 * the source learns nothing from a correct guess. */
static bool host_punch_gate_is_muted(const char* ip, uint32_t now_ms) {
    for (int i = 0; i < HOST_PUNCH_SRC_TABLE; i++) {
        const HostPunchSrc* e = &s_host_punch_src[i];
        if (e->ip[0] == '\0' || strcmp(e->ip, ip) != 0) continue;
        return e->mute_until != 0 && (int32_t)(now_ms - e->mute_until) < 0;
    }
    return false;
}

/* Charge one bad punch-shaped datagram to `ip`. Returns true when this
 * charge crossed the session re-roll threshold and a re-roll is owed
 * (the caller performs it — this function stays free of s_work and
 * thread lifecycle concerns so it can be unit-tested). */
static bool host_punch_gate_note_bad(const char* ip, uint32_t now_ms) {
    s_host_punch_bad_total++;

    HostPunchSrc* e = host_punch_src_slot(ip, now_ms);
    if (e != NULL && e->mute_until == 0) {
        e->bad++;
        if (e->bad >= HOST_PUNCH_SRC_MAX_BAD) {
            e->mute_until = now_ms + HOST_PUNCH_MUTE_MS;
            if (e->mute_until == 0) e->mute_until = 1; /* 0 means "not muted" */
            SDL_Log("[direct_p2p] %s punch-gate MUTE %s for %u ms after %d "
                    "bad-token punches — that source can no longer probe the "
                    "gate (session total %d)",
                    ConnectFail_Code(CONNECT_FAIL_PUNCH_AUTH), ip,
                    (unsigned)HOST_PUNCH_MUTE_MS, e->bad,
                    s_host_punch_bad_total);
        }
    }

    return s_host_punch_bad_total >= HOST_PUNCH_TOTAL_REROLL &&
           s_host_punch_rerolls < HOST_PUNCH_REROLL_MAX;
}

/* --- S4c: rendezvous return-routability cookie (host side) ------------- */

/* The v2 rendezvous server answers an uncookied REGISTER with a
 * CHALLENGE and only binds a slot once the client echoes the cookie
 * back (proving it receives at its claimed source). On the HOST the
 * writer and reader live on different threads: the CHALLENGE arrives
 * on the MAIN thread (host_tick_receive -> host_handle_challenge)
 * while the periodic REGISTER resends are built on the rendezvous
 * WORKER thread. A seqlock publishes the 8-byte cookie across:
 * seq == 0 means "no cookie yet"; the writer bumps to odd, writes the
 * bytes, bumps to even; the reader snapshots seq, copies, re-checks.
 * The JOINER needs none of this — its signaling loop is inline in the
 * join worker, the socket's sole actor. */
static uint8_t s_signal_cookie[REND_COOKIE_LEN];
static SDL_AtomicInt s_signal_cookie_seq = { 0 };

/* Main-thread writer (host_handle_challenge). */
static void signal_cookie_publish(const uint8_t cookie[REND_COOKIE_LEN]) {
    int seq = SDL_GetAtomicInt(&s_signal_cookie_seq);
    if (seq & 1) seq++; /* defensive: never leave an odd value behind */
    SDL_SetAtomicInt(&s_signal_cookie_seq, seq + 1);       /* odd: writing */
    memcpy(s_signal_cookie, cookie, REND_COOKIE_LEN);
    SDL_SetAtomicInt(&s_signal_cookie_seq, seq + 2);       /* even: stable */
}

static void signal_cookie_reset(void) {
    SDL_SetAtomicInt(&s_signal_cookie_seq, 0);
    memset(s_signal_cookie, 0, sizeof(s_signal_cookie));
}

/* Worker-thread reader. Returns true and fills `out` when a stable
 * cookie is available; false -> caller sends the uncookied form. */
static bool signal_cookie_snapshot(uint8_t out[REND_COOKIE_LEN]) {
    for (int attempt = 0; attempt < 4; attempt++) {
        const int s1 = SDL_GetAtomicInt(&s_signal_cookie_seq);
        if (s1 == 0 || (s1 & 1)) return false; /* none, or mid-write */
        memcpy(out, s_signal_cookie, REND_COOKIE_LEN);
        if (SDL_GetAtomicInt(&s_signal_cookie_seq) == s1) return true;
    }
    return false; /* writer kept racing us — next tick will get it */
}

/* S4c: has the host seen a CHALLENGE this hosting session? "Server is
 * alive" evidence for the host-waiting advisory (a challenge with no
 * subsequent DELIVER points at cookie/auth trouble, not a dead
 * server). Main-thread only. */
static bool s_host_challenge_seen = false;

/* Record the taxonomy code and put its user string on the overlay status
 * line. Callable from worker threads (same rules as set_status — s_work
 * writes happen-before the terminal state publish). */
static void set_fail(ConnectFailCode code) {
    s_work.fail_code = code;
    set_status(ConnectFail_UserText(code));
}

/* Like set_fail but with a custom status string (cases where the generic
 * user text would mislead, e.g. kill-switch bypass). */
static void set_fail_msg(ConnectFailCode code, const char* msg) {
    s_work.fail_code = code;
    set_status(msg);
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

/* Classify a router-reported external IP for the CGNAT gate (review
 * M3). Returns true — "the mapping is provably useless" — only when
 * the string PARSES as IPv4 and is a non-public address: RFC1918,
 * RFC 6598 CGN shared space (100.64.0.0/10), loopback, or link-local.
 * A public-but-different IP (ISP 1:1 NAT / DMZ, where the router's WAN
 * IP differs from the STUN IP but the mapping forwards perfectly) and
 * an unparseable string (garbage, IPv6 form) must NOT poison a working
 * mapping — callers keep it and log instead. */
static bool direct_p2p_ip_is_nonpublic(const char* ip) {
    if (direct_p2p_is_lan_peer(ip)) {
        return true; /* 127/8, 10/8, 172.16/12, 192.168/16, 169.254/16 */
    }
    struct in_addr addr;
    if (!ip || inet_pton(AF_INET, ip, &addr) != 1) {
        return false; /* unparseable — cannot prove anything, keep mapping */
    }
    uint32_t a = ntohl(addr.s_addr);
    /* 100.64.0.0/10 — RFC 6598 carrier-grade NAT shared address space */
    if ((a & 0xFFC00000u) == 0x64400000u) return true;
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
 *   bool Stun_HolePunch(StunResult*, char*, uint16_t*, const uint8_t[8],
 *                       int, SDL_AtomicInt*);
 *   bool Rendezvous_Send(NET_DatagramSocket*, NET_Address*, uint16_t,
 *                        const uint8_t*, size_t);
 */
#ifdef NETPLAY_TEST_HOOKS
/* Public typedefs are declared in direct_p2p.h. The local function
 * pointers default to the production functions; test_bilateral_punch.c
 * overrides them via DirectP2P_TestHook_Set*. */
static DirectP2P_StunHolePunch_fn  s_stun_hole_punch_impl  = Stun_HolePunch;
static DirectP2P_RendezvousSend_fn s_rendezvous_send_impl  = Rendezvous_Send;
/* S2: Stun_Discover seam so the joiner auto-retry test can drive
 * BeginJoin end-to-end without touching real STUN servers. */
static DirectP2P_StunDiscover_fn   s_stun_discover_impl    = Stun_Discover;
/* S3-review HIGH-1: signaling-budget override so the self-DELIVER
 * regression test doesn't spend 2 x 8 s (both attempts' full default
 * budget) waiting out a loop whose early-exit the fix deliberately
 * removes. 0 = use the config value. */
static int s_test_signal_budget_ms = 0;

#define STUN_HOLE_PUNCH(stun, peer_ip, peer_port, token, duration_ms, cancel) \
    s_stun_hole_punch_impl((stun), (peer_ip), (peer_port), (token), (duration_ms), (cancel))
#define RENDEZVOUS_SEND(sock, target, target_port, pkt, pkt_len) \
    s_rendezvous_send_impl((sock), (target), (target_port), (pkt), (pkt_len))
#define STUN_DISCOVER(result, local_port, timeout_ms) \
    s_stun_discover_impl((result), (local_port), (timeout_ms))

void DirectP2P_TestHook_SetStunHolePunch(DirectP2P_StunHolePunch_fn fn) {
    s_stun_hole_punch_impl = (fn != NULL) ? fn : Stun_HolePunch;
}
void DirectP2P_TestHook_SetRendezvousSend(DirectP2P_RendezvousSend_fn fn) {
    s_rendezvous_send_impl = (fn != NULL) ? fn : Rendezvous_Send;
}
void DirectP2P_TestHook_SetStunDiscover(DirectP2P_StunDiscover_fn fn) {
    s_stun_discover_impl = (fn != NULL) ? fn : Stun_Discover;
}
void DirectP2P_TestHook_SetSignalBudgetMs(int ms) {
    s_test_signal_budget_ms = (ms > 0) ? ms : 0;
}
bool DirectP2P_TestHook_IsLanPeer(const char* ip) {
    return direct_p2p_is_lan_peer(ip);
}
#else
#define STUN_HOLE_PUNCH(stun, peer_ip, peer_port, token, duration_ms, cancel) \
    Stun_HolePunch((stun), (peer_ip), (peer_port), (token), (duration_ms), (cancel))
#define RENDEZVOUS_SEND(sock, target, target_port, pkt, pkt_len) \
    Rendezvous_Send((sock), (target), (target_port), (pkt), (pkt_len))
#define STUN_DISCOVER(result, local_port, timeout_ms) \
    Stun_Discover((result), (local_port), (timeout_ms))
#endif /* NETPLAY_TEST_HOOKS */

/* --- S4a: host-waiting datagram classifier ----------------------------- */

/* One routing decision per inbound datagram on the host's waiting
 * socket. Order matters: rendezvous frames and STUN responses are
 * infrastructure traffic recognized by structure; ONLY a datagram
 * carrying the exact authenticated punch payload may be accepted as
 * the peer. Everything else — port scans, stray packets, legacy
 * unauthenticated punches, spoofed garbage — is IGNORE: the host drops
 * it and KEEPS WAITING. (Pre-S4a the final arm was "anything else IS
 * the peer": one stray datagram consumed the host's only peer slot,
 * the real joiner then failed, and after the MIST silence window the
 * user got the misleading "opponent build may be too old".) */
static DirectP2PHostDgramClass classify_host_datagram(
        const uint8_t* buf, int len,
        const uint8_t token[STUN_PUNCH_TOKEN_LEN], bool token_valid) {
    if (buf == NULL || len <= 0) {
        return DP2P_HOST_DGRAM_IGNORE;
    }
    /* Rendezvous DELIVER/CHALLENGE: 32+ bytes, '3SXR' magic
     * (0x33535852 BE). Hosts only ever receive server->client frames;
     * REGISTER and POLL are client->server only. Gate BEFORE the punch
     * check so a server frame is never mistaken for a peer probe. */
    if (len >= 32 && buf[0] == 0x33 && buf[1] == 0x53 &&
        buf[2] == 0x58 && buf[3] == 0x52) {
        return DP2P_HOST_DGRAM_RENDEZVOUS;
    }
    /* S1: STUN Binding Responses (keepalive replies, or a late
     * duplicate from a slower Stun_Discover server). */
    if (Stun_IsBindingResponse(buf, len)) {
        return DP2P_HOST_DGRAM_STUN;
    }
    /* S4a: the peer slot requires the exact 17-byte authenticated
     * punch payload. Fail closed when no token exists. */
    if (token_valid && Stun_IsPunchPayload(buf, len, token)) {
        return DP2P_HOST_DGRAM_PEER_PUNCH;
    }
    return DP2P_HOST_DGRAM_IGNORE;
}

#ifdef NETPLAY_TEST_HOOKS
DirectP2PHostDgramClass DirectP2P_TestHook_ClassifyHostDatagram(
        const uint8_t* buf, int len,
        const uint8_t token[STUN_PUNCH_TOKEN_LEN], bool token_valid) {
    return classify_host_datagram(buf, len, token, token_valid);
}
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
    uint8_t      payload[REND_REGISTER_PKT_LEN]; /* v2 REGISTER or POLL packet */
    uint8_t      payload_len;  /* always 36 in practice; future-proof */
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
        uint8_t payload[REND_REGISTER_PKT_LEN];
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

/* S2: CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS is the overall
 * wall-clock budget for Stun_Discover (all servers probed in parallel;
 * see stun.c). Read at each discovery site via stun_budget_ms().
 *
 * Ceiling rationale (review L-4): Stun_Discover takes no cancel flag —
 * it runs to its budget when no server answers — and DirectP2P_Cancel
 * joins the worker on the GAME thread, so this budget is a direct
 * bound on how long Cancel can block the game. The retransmit ladder
 * ends 1500 ms after the last server arms; 15 s already covers any
 * network where discovery can succeed at all. An uncapped user value
 * would turn a mid-discovery Cancel into an arbitrarily long UI
 * freeze. */
static int stun_budget_ms(void) {
    int ms = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS);
    if (ms <= 0) ms = 4000;      /* keep in sync with the config.c default */
    if (ms < 1000) ms = 1000;    /* below one RTO the retransmit ladder is meaningless */
    if (ms > 15000) ms = 15000;  /* ceiling — bounds DirectP2P_Cancel's worst-case block */
    return ms;
}

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
    if (!Rendezvous_DeriveSessionKey(ip_be, s_work.advertised_port, s_work.nonce,
                                     session_key)) {
        SDL_Log("[direct_p2p] rendezvous: failed to derive session key");
        NET_UnrefAddress(signal_addr);
        return 0;
    }

    /* 4) REGISTER packets are (re)built per send since S4c: the cookie
     * tail changes when the main thread answers a server CHALLENGE
     * (signal_cookie_publish) or when the server rotates its cookie
     * slot. Validate buildability once up front so a hard failure
     * still exits early. */
    uint8_t register_pkt[REND_REGISTER_PKT_LEN];
    if (!Rendezvous_BuildRegister(s_work.stun.public_port, session_key, NULL,
                                  register_pkt)) {
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
    /* S3: REGISTERs are now actually flowing — arm the cause-8 advisory
     * (see s_host_registering). */
    SDL_SetAtomicInt(&s_host_registering, 1);
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
            /* S4c: rebuild with the latest cookie (zeros until the main
             * thread has answered a CHALLENGE; the server treats an
             * uncookied REGISTER as a challenge request, not a bind). */
            uint8_t cookie[REND_COOKIE_LEN];
            const bool have_cookie = signal_cookie_snapshot(cookie);
            (void)Rendezvous_BuildRegister(s_work.stun.public_port, session_key,
                                           have_cookie ? cookie : NULL,
                                           register_pkt);
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
    if (budget_ms <= 0) budget_ms = 5000; /* S2: keep in sync with config.c default */

    set_status("Connecting...");
    SDL_Log("[direct_p2p] entering FALLBACK_BILATERAL_PUNCH peer=%s:%u (budget=%dms)",
            peer_ip, (unsigned)peer_port, budget_ms);

    const uint32_t stage_t0 = SDL_GetTicks();
    /* S4a: the host punches with the token derived from its own
     * advertised tuple (host_thread_fn refuses to advertise without
     * one, so punch_token_valid is guaranteed here). */
    bool punched = STUN_HOLE_PUNCH(&s_work.stun, peer_ip, &peer_port,
                                   s_work.punch_token,
                                   budget_ms, &s_bilateral_punch_cancel);
    s_work.t_bilateral_ms = SDL_GetTicks() - stage_t0;
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
     * an external port (some firmware rejects wildcards, error 716).
     *
     * S2-retry hygiene (review L-5): on a FAILED_STUN auto-retry, a
     * PREVIOUS attempt in this hosting session may already hold a live
     * mapping (s_upnp_mapping is only reset in DirectP2P_Init /
     * teardown, deliberately — the router-side lease survives the
     * retry). Re-probing could only refresh that same (internal,
     * external) pair, but a TRANSIENT re-probe failure would leave
     * upnp_ok=false while s_upnp_mapping.active stays true — the
     * advertised port becomes the STUN port while a later room-code
     * drift re-encode (which keys off s_upnp_mapping.active) would
     * switch to the stale mapping's port. Skip the re-probe and trust
     * the live mapping instead: consistent with S1's renewal policy,
     * which likewise KEEPS the mapping when a renew attempt fails
     * (upnp_renew_tick) rather than tearing it down on a blip. */
    set_state(DIRECT_P2P_UPNP_PROBE);
    uint32_t stage_t0 = SDL_GetTicks();
    bool upnp_ok;
    if (s_upnp_mapping.active && s_upnp_mapping.internal_port == local_port) {
        SDL_Log("[direct_p2p] retry: reusing the live UPnP mapping from the previous "
                "attempt (external %u -> internal %u) — skipping re-probe",
                s_upnp_mapping.external_port, s_upnp_mapping.internal_port);
        upnp_ok = true;
    } else {
        upnp_ok = try_upnp(local_port, local_port);
    }
    s_work.t_upnp_ms = SDL_GetTicks() - stage_t0;
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
    stage_t0 = SDL_GetTicks();
    bool stun_ok = STUN_DISCOVER(&s_work.stun, local_port, stun_budget_ms());
    s_work.t_stun_ms = SDL_GetTicks() - stage_t0;
    if (cancel_requested()) {
        Stun_CloseSocket(&s_work.stun);
        set_status("Cancelled.");
        set_state(DIRECT_P2P_IDLE);
        return 0;
    }
    if (!stun_ok) {
        /* S3 causes 1-2 (host side): same classification as the joiner.
         * Review M-2: a local socket-creation failure is not a network
         * condition — classify INTERNAL, not "no internet". S4-review
         * L-3: a CSPRNG failure is the same shape — nothing was ever
         * sent, so a connectivity diagnosis would be a lie. */
        set_fail((s_work.stun.diag_socket_fail || s_work.stun.diag_csprng_fail)
                     ? CONNECT_FAIL_INTERNAL
                     : ConnectFail_ClassifyStunDiscover(
                           s_work.stun.diag_servers_probed,
                           s_work.stun.diag_servers_answered,
                           s_work.stun.diag_sends_ok,
                           s_work.stun.diag_dns_all_failed));
        set_state(DIRECT_P2P_FAILED_STUN);
        return 0;
    }

    /* S1 CGNAT gate: on carrier-grade NAT (or any double NAT) the inner
     * router happily grants a mapping whose external IP is a private /
     * CGN (100.64/10) address, while STUN reports the true public IP.
     * The room code is built from the STUN IP + the chosen port, so
     * advertising the UPnP port would pair the STUN IP with a port that
     * only exists on the INNER router — a wrong (ip, port) pair that
     * silently kills the direct path.
     *
     * Review M3: the mapping is dropped only when the router's external
     * IP PARSES and is provably non-public (RFC1918 / CGN 100.64.0.0/10 /
     * loopback / link-local) — that is the double-NAT signature. A
     * public-but-different external IP is ISP 1:1 NAT / DMZ territory,
     * where the mapping forwards perfectly and dropping it would
     * downgrade a host with a good direct path; an unparseable string
     * (garbage, IPv6) proves nothing. Both keep the mapping and log. */
    if (upnp_ok &&
        !direct_p2p_ip_eq_normalized(s_upnp_mapping.external_ip, s_work.stun.public_ip)) {
        if (direct_p2p_ip_is_nonpublic(s_upnp_mapping.external_ip)) {
            SDL_Log("[direct_p2p] CGNAT detected: UPnP external IP %s is private/CGN and "
                    "!= STUN public IP %s — ignoring the UPnP mapping and advertising "
                    "the STUN endpoint instead",
                    s_upnp_mapping.external_ip, s_work.stun.public_ip);
            /* Release the useless mapping now (worker thread — same thread
             * class try_upnp used; no concurrent miniupnpc user exists in
             * UPNP_PROBE/STUN_DISCOVER states). Also stops the S1 lease
             * renewal from ever arming for it. */
            Upnp_RemoveMapping(&s_upnp_mapping);
            memset(&s_upnp_mapping, 0, sizeof(s_upnp_mapping));
            upnp_ok = false;
        } else {
            SDL_Log("[direct_p2p] UPnP external IP %s differs from STUN public IP %s but "
                    "is not a private/CGN address (1:1 NAT / DMZ, or unparseable) — "
                    "keeping the mapping",
                    s_upnp_mapping.external_ip[0] ? s_upnp_mapping.external_ip : "(empty)",
                    s_work.stun.public_ip);
        }
    }

    /* 3) Build room code from discovered endpoint. If UPnP succeeded we
     * prefer its external port over the STUN-observed port — symmetric
     * NATs translate per-destination, so the STUN port is only valid
     * for STUN servers; the UPnP mapping is stable for any peer. The
     * room code no longer carries the host's LAN local_port; see
     * room_code.h for the rationale. */
    uint16_t pub_port = upnp_ok ? s_upnp_mapping.external_port : s_work.stun.public_port;
    uint32_t ip_be = ipv4_str_to_be(s_work.stun.public_ip);
    /* S4b: fresh CSPRNG nonce per hosting attempt — mixed into the code
     * payload, the rendezvous session key, and the punch token, so
     * neither can be derived from the observable (ip, port) alone.
     * Fail closed on CSPRNG failure: a predictable nonce would silently
     * void the protection (see RoomCode_GenerateNonce). */
    if (!RoomCode_GenerateNonce(&s_work.nonce)) {
        SDL_Log("[direct_p2p] CSPRNG unavailable — cannot generate a room-code nonce");
        Stun_CloseSocket(&s_work.stun);
        set_fail(CONNECT_FAIL_INTERNAL);
        set_state(DIRECT_P2P_FAILED_STUN);
        return 0;
    }
    if (ip_be == 0 ||
        !RoomCode_Encode(ip_be, pub_port, s_work.nonce, s_work.host_code)) {
        Stun_CloseSocket(&s_work.stun);
        set_fail(CONNECT_FAIL_INTERNAL);
        set_state(DIRECT_P2P_FAILED_STUN);
        return 0;
    }
    /* Record the advertised tuple's port — the rendezvous session key is
     * derived from (public_ip, advertised_port) on both roles (S1). Must
     * be visible before HOST_WAITING publishes (the rendezvous thread
     * spawns after and reads it). */
    s_work.advertised_port = pub_port;

    /* S4a: derive the punch-auth token from the SAME advertised tuple
     * the room code encodes — the joiner derives the identical token
     * from the decoded code. Fail closed: without a token the host's
     * datagram gate would ignore every punch, so an unjoinable room
     * must not be advertised at all. */
    s_work.punch_token_valid =
        Rendezvous_DerivePunchToken(ip_be, pub_port, s_work.nonce, s_work.punch_token);
    if (!s_work.punch_token_valid) {
        Stun_CloseSocket(&s_work.stun);
        set_fail(CONNECT_FAIL_INTERNAL);
        set_state(DIRECT_P2P_FAILED_STUN);
        return 0;
    }

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
    {
        /* MEDIUM-4: the code is key material now — nonce chars redacted.
         * The ip:port on the same line is the non-secret half. */
        char code_redacted[ROOM_CODE_BUF_LEN];
        RoomCode_Redact(s_work.host_code, code_redacted);
        SDL_Log("[direct_p2p] HOST_WAITING published. Code=%s (nonce redacted) "
                "public=%s:%u (via %s)",
                code_redacted, s_work.stun.public_ip, (unsigned)pub_port,
                upnp_ok ? "UPnP" : "STUN");
    }

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

/* Join attempt: STUN discover -> hairpin-detect -> Stun_HolePunch ->
 * bilateral fallback. One complete pass of the joiner flow. RETURNS the
 * terminal state instead of publishing it — the join_thread_fn wrapper
 * below owns the terminal set_state so it can interpose the S2
 * auto-retry (docs/plan-netplay-connection.md §4). Intermediate states
 * (STUN_DISCOVER / JOIN_PUNCHING / FALLBACK_*) are still published
 * here; every failure return has already closed the attempt's socket
 * and set the status text; DIRECT_P2P_HANDOFF returns with s_work
 * fully written back. */
static DirectP2PState join_attempt(void) {
    /* S3: fresh evidence per attempt — the retry's classification and
     * report must not inherit the first attempt's DELIVER counters or
     * the timings of stages the retry never reached. */
    s_work.fail_code = CONNECT_FAIL_NONE;
    s_work.ev_deliver_any = false;
    s_work.ev_deliver_real = false;
    s_work.ev_challenge_any = false; /* S4c */
    s_work.t_stun_ms = 0;
    s_work.t_punch_ms = 0;
    s_work.t_signal_ms = 0;
    s_work.t_bilateral_ms = 0;
    s_work.join_attempts++;

    set_state(DIRECT_P2P_STUN_DISCOVER);
    set_status("Preparing...");
    uint32_t stage_t0 = SDL_GetTicks();
    bool stun_ok = STUN_DISCOVER(&s_work.stun, 0, stun_budget_ms());
    s_work.t_stun_ms = SDL_GetTicks() - stage_t0;
    if (cancel_requested()) {
        Stun_CloseSocket(&s_work.stun);
        set_status("Cancelled.");
        return DIRECT_P2P_IDLE;
    }
    if (!stun_ok) {
        /* S3 causes 1-2: distinguish "no network / DNS dead" from
         * "outbound UDP filtered" using the discovery evidence.
         * Review M-2: a local socket-creation failure is neither —
         * classify INTERNAL, not "no internet". */
        set_fail((s_work.stun.diag_socket_fail || s_work.stun.diag_csprng_fail)
                     ? CONNECT_FAIL_INTERNAL
                     : ConnectFail_ClassifyStunDiscover(
                           s_work.stun.diag_servers_probed,
                           s_work.stun.diag_servers_answered,
                           s_work.stun.diag_sends_ok,
                           s_work.stun.diag_dns_all_failed));
        return DIRECT_P2P_FAILED_STUN;
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

    /* S4a: derive the punch-auth token from the DECODED room-code tuple
     * (the host derives the identical token from its advertised tuple).
     * Derived once per attempt; used for the direct punch and, on
     * fallback, the bilateral punch. Never overwritten by retargeting —
     * the token is bound to the advertised payload, not to whatever
     * translated endpoint the punch later observes. */
    uint8_t punch_token[STUN_PUNCH_TOKEN_LEN];
    {
        uint32_t token_host_ip_be = ipv4_str_to_be(s_work.peer_ip);
        if (!Rendezvous_DerivePunchToken(token_host_ip_be, s_work.peer_public_port,
                                         s_work.nonce, punch_token)) {
            Stun_CloseSocket(&s_work.stun);
            set_fail(CONNECT_FAIL_INTERNAL);
            return DIRECT_P2P_FAILED_STUN;
        }
    }

    /* Hole-punch. 2.5s window per upstream. Stun_HolePunch updates the
     * peer_ip/peer_port in place with the translated endpoint the host
     * actually reaches us from. */
    set_state(DIRECT_P2P_JOIN_PUNCHING);
    set_status("Connecting...");
    char punch_peer_ip[64];
    SDL_strlcpy(punch_peer_ip, s_work.peer_ip, sizeof(punch_peer_ip));
    uint16_t punch_peer_port = s_work.peer_public_port;
    stage_t0 = SDL_GetTicks();
    bool punched = STUN_HOLE_PUNCH(&s_work.stun, punch_peer_ip, &punch_peer_port,
                                   punch_token, 2500, &s_cancel);
    s_work.t_punch_ms = SDL_GetTicks() - stage_t0;
    if (cancel_requested()) {
        Stun_CloseSocket(&s_work.stun);
        set_status("Cancelled.");
        return DIRECT_P2P_IDLE;
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
        /* S4a: bad-token evidence is terminal — the host's datagrams
         * REACHED us but failed authentication, so the bilateral punch
         * (same token, same peer) would fail identically. Surfacing
         * PUNCH_AUTH now beats burning the signaling+bilateral budgets
         * to then blame NAT for a version/payload mismatch. */
        if (s_work.stun.diag_punch_bad_token) {
            Stun_CloseSocket(&s_work.stun);
            set_fail(CONNECT_FAIL_PUNCH_AUTH);
            return DIRECT_P2P_FAILED_SYMMETRIC;
        }
        if (Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL)) {
            Stun_CloseSocket(&s_work.stun);
            set_fail_msg(CONNECT_FAIL_NAT_BLOCKED,
                         "Direct punch failed (fallback disabled).");
            return DIRECT_P2P_FAILED_SYMMETRIC;
        }
        if (direct_p2p_is_lan_peer(s_work.peer_ip)) {
            Stun_CloseSocket(&s_work.stun);
            set_fail_msg(CONNECT_FAIL_NAT_BLOCKED,
                         "LAN peer unreachable (check firewall).");
            return DIRECT_P2P_FAILED_SYMMETRIC;
        }
        if (s_work.stun.public_ip[0] != '\0' &&
            direct_p2p_ip_eq_normalized(s_work.peer_ip, s_work.stun.public_ip)) {
            /* S3 cause 7: peer public IP == our public IP — same router,
             * and the direct punch already proved it lacks NAT loopback.
             * The bilateral punch would loop through the same broken
             * router, so this is terminal here. */
            Stun_CloseSocket(&s_work.stun);
            set_fail(CONNECT_FAIL_HAIRPIN);
            return DIRECT_P2P_FAILED_SYMMETRIC;
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
            set_fail(CONNECT_FAIL_INTERNAL);
            return DIRECT_P2P_FAILED_BILATERAL;
        }
        char signal_host[64] = { 0 };
        uint16_t signal_port = 0;
        if (!Rendezvous_ParseSignalUrl(signal_url, signal_host, &signal_port)) {
            SDL_Log("[direct_p2p] joiner fallback: malformed signal URL '%s'", signal_url);
            Stun_CloseSocket(&s_work.stun);
            set_fail(CONNECT_FAIL_INTERNAL);
            return DIRECT_P2P_FAILED_BILATERAL;
        }

        /* 2) Resolve hostname (100ms-bounded poll, mirror stun.c:272-275). */
        NET_Address* signal_addr = resolve_with_short_poll(signal_host);
        if (!signal_addr) {
            SDL_Log("[direct_p2p] joiner fallback: failed to resolve %s", signal_host);
            Stun_CloseSocket(&s_work.stun);
            /* DNS worked for STUN moments ago; a dead resolve HERE most
             * likely means the rendezvous hostname itself is gone. */
            set_fail(CONNECT_FAIL_RENDEZVOUS_DOWN);
            return DIRECT_P2P_FAILED_BILATERAL;
        }

        /* 3) Derive session key from the HOST's public endpoint, decoded
         * from the room code into peer_ip / peer_public_port. The host
         * registered with the rendezvous using the same hash, and the
         * server pairs entries by literal session_key equality — so both
         * sides MUST hash the host's tuple to wind up in the same slot. */
        uint8_t session_key[16];
        uint32_t host_ip_be = ipv4_str_to_be(s_work.peer_ip);
        if (!Rendezvous_DeriveSessionKey(host_ip_be, s_work.peer_public_port,
                                         s_work.nonce, session_key)) {
            SDL_Log("[direct_p2p] joiner fallback: failed to derive session key");
            NET_UnrefAddress(signal_addr);
            Stun_CloseSocket(&s_work.stun);
            set_fail(CONNECT_FAIL_INTERNAL);
            return DIRECT_P2P_FAILED_BILATERAL;
        }

        /* 4) Build the initial (uncookied) REGISTER. S4c: the server
         * answers it with a CHALLENGE; the loop below echoes the cookie
         * and rebuilds the packet, after which REGISTERs bind normally. */
        uint8_t register_pkt[REND_REGISTER_PKT_LEN];
        uint8_t signal_cookie[REND_COOKIE_LEN] = { 0 };
        bool have_signal_cookie = false;
        if (!Rendezvous_BuildRegister(s_work.stun.public_port, session_key, NULL,
                                      register_pkt)) {
            SDL_Log("[direct_p2p] joiner fallback: failed to build REGISTER packet");
            NET_UnrefAddress(signal_addr);
            Stun_CloseSocket(&s_work.stun);
            set_fail(CONNECT_FAIL_INTERNAL);
            return DIRECT_P2P_FAILED_BILATERAL;
        }

        /* 5) Inline REGISTER/POLL + DELIVER receive loop. 500ms send
         * cadence; 8s default budget (configurable). 50ms inner sleep so
         * the cancel check has ~50ms granularity. Direct Rendezvous_Send
         * is safe here because this thread is the sole reader/writer of
         * the STUN socket. */
        int signal_budget_ms = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_BUDGET_MS);
        if (signal_budget_ms <= 0) signal_budget_ms = 8000;
#ifdef NETPLAY_TEST_HOOKS
        if (s_test_signal_budget_ms > 0) signal_budget_ms = s_test_signal_budget_ms;
#endif
        int bilateral_budget_ms = Config_GetInt(CFG_KEY_NETPLAY_DIRECT_P2P_BILATERAL_PUNCH_MS);
        if (bilateral_budget_ms <= 0) bilateral_budget_ms = 5000; /* S2: sync w/ config.c */

        char fb_peer_ip[64] = { 0 };
        uint16_t fb_peer_port = 0;
        bool got_deliver = false;

        const uint32_t signal_start = SDL_GetTicks();
        stage_t0 = signal_start;
        uint32_t last_send = 0;
        while ((int)(SDL_GetTicks() - signal_start) < signal_budget_ms) {
            if (cancel_requested()) {
                NET_UnrefAddress(signal_addr);
                Stun_CloseSocket(&s_work.stun);
                set_status("Cancelled.");
                return DIRECT_P2P_IDLE;
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
                    dgram->buf[2] == 0x58 && dgram->buf[3] == 0x52 &&
                    dgram->buf[5] == 4 /* REND_TYPE_CHALLENGE */) {
                    /* S4c: server challenge — echo the cookie right away
                     * (one RTT to bind instead of the 500 ms resend
                     * cadence) and carry it on every later resend. A
                     * challenge is ALSO liveness evidence: the server
                     * answered us, so a budget expiry with challenges
                     * but no DELIVERs is an auth problem, not a dead
                     * server. */
                    if (Rendezvous_ParseChallenge(dgram->buf, dgram->buflen,
                                                  session_key, signal_cookie)) {
                        have_signal_cookie = true;
                        s_work.ev_challenge_any = true;
                        (void)Rendezvous_BuildRegister(s_work.stun.public_port,
                                                       session_key, signal_cookie,
                                                       register_pkt);
                        (void)RENDEZVOUS_SEND(s_work.stun.socket, signal_addr,
                                              signal_port, register_pkt,
                                              sizeof(register_pkt));
                        SDL_Log("[direct_p2p] joiner answered rendezvous CHALLENGE");
                    }
                    (void)have_signal_cookie;
                } else if (dgram->buflen >= 32 &&
                    dgram->buf[0] == 0x33 && dgram->buf[1] == 0x53 &&
                    dgram->buf[2] == 0x58 && dgram->buf[3] == 0x52) {
                    char parsed_ip[64] = { 0 };
                    uint16_t parsed_port = 0;
                    /* S3: tri-state parse. A zero-sentinel DELIVER is
                     * MEANINGFUL evidence — it proves the rendezvous
                     * server is alive (it answers every REGISTER), which
                     * separates "server down" from "host offline" when
                     * the budget expires below. */
                    const RendezvousDeliverResult dr = Rendezvous_ParseDeliverEx(
                        dgram->buf, dgram->buflen, session_key, parsed_ip, &parsed_port);
                    if (dr != REND_DELIVER_MALFORMED) {
                        s_work.ev_deliver_any = true;
                    }
                    if (dr == REND_DELIVER_PEER &&
                        parsed_ip[0] != '\0' && parsed_port != 0) {
                        /* S3-review HIGH-1 — joiner self-DELIVER gate, the
                         * sibling of the host's H1 gate in
                         * try_handle_deliver. A DELIVER whose endpoint IP
                         * equals OUR OWN public IP is not the host: it is
                         * this client's own registration from a previous
                         * attempt, echoed back after the S2 fresh-socket
                         * retry re-REGISTERed from a new source port and
                         * the server filed us as "the joiner" of our own
                         * attempt-1 slot. A LEGITIMATE same-IP host is
                         * impossible here: the hairpin bypass above
                         * (ip_eq_normalized(peer_ip, public_ip) before
                         * FALLBACK_SIGNALING) fails any same-IP room code
                         * with FAILED_SYMMETRIC before this loop ever
                         * REGISTERs — the same argument the server's
                         * SLOT_STALE_MS comment makes. Pre-fix this
                         * consumed the self-endpoint as a live host,
                         * punched our own dead attempt-1 mapping, and
                         * misreported the single most common real failure
                         * (a stale room code) as NAT_BLOCKED /
                         * SYMMETRIC_BOTH instead of HOST_OFFLINE. Treat it
                         * as EMPTY evidence (server alive, host absent)
                         * and keep polling. */
                        if (s_work.stun.public_ip[0] != '\0' &&
                            direct_p2p_ip_eq_normalized(parsed_ip,
                                                        s_work.stun.public_ip)) {
                            SDL_Log("[direct_p2p] joiner DELIVER carries our own "
                                    "public IP (%s:%u) — our stale registration "
                                    "from a previous attempt, not the host; "
                                    "treating as 'peer not registered'",
                                    parsed_ip, (unsigned)parsed_port);
                        } else {
                            s_work.ev_deliver_real = true;
                            SDL_strlcpy(fb_peer_ip, parsed_ip, sizeof(fb_peer_ip));
                            fb_peer_port = parsed_port;
                            got_deliver = true;
                            SDL_Log("[direct_p2p] joiner DELIVER received peer=%s:%u",
                                    fb_peer_ip, (unsigned)fb_peer_port);
                        }
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
        s_work.t_signal_ms = SDL_GetTicks() - stage_t0;

        if (!got_deliver) {
            /* S3 causes 3-4: silence from the server (which answers every
             * REGISTER) means the server/path is down; only-sentinel
             * replies mean the HOST never registered — offline or a
             * stale code. */
            ConnectJoinEvidence ev = { 0 };
            ev.deliver_any = s_work.ev_deliver_any;
            ev.deliver_real = s_work.ev_deliver_real;
            ev.challenge_any = s_work.ev_challenge_any; /* S4c */
            ev.port_disagreement = s_work.stun.port_disagreement;
            const ConnectFailCode jc = ConnectFail_ClassifyJoin(&ev);
            SDL_Log("[direct_p2p] joiner fallback: signal budget expired without a "
                    "peer DELIVER (deliver_any=%d challenge_any=%d) -> %s",
                    (int)s_work.ev_deliver_any, (int)s_work.ev_challenge_any,
                    ConnectFail_Code(jc));
            Stun_CloseSocket(&s_work.stun);
            set_fail(jc);
            return DIRECT_P2P_FAILED_BILATERAL;
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

        stage_t0 = SDL_GetTicks();
        bool bilateral_punched = STUN_HOLE_PUNCH(&s_work.stun, bp_peer_ip,
                                                 &bp_peer_port, punch_token,
                                                 bilateral_budget_ms, &s_cancel);
        s_work.t_bilateral_ms = SDL_GetTicks() - stage_t0;
        if (cancel_requested()) {
            Stun_CloseSocket(&s_work.stun);
            set_status("Cancelled.");
            return DIRECT_P2P_IDLE;
        }
        if (!bilateral_punched) {
            /* S3 causes 5-6: the server handed us the host's LIVE
             * endpoint and the bilateral punch still timed out — the
             * NAT pair is the blocker. Our own S2 port_disagreement
             * signal upgrades this to the needs-relay class. */
            ConnectJoinEvidence ev = { 0 };
            ev.deliver_any = s_work.ev_deliver_any;
            ev.deliver_real = s_work.ev_deliver_real;
            ev.challenge_any = s_work.ev_challenge_any; /* S4c */
            ev.bilateral_punched = false;
            ev.port_disagreement = s_work.stun.port_disagreement;
            ev.punch_bad_token = s_work.stun.diag_punch_bad_token; /* S4a */
            const ConnectFailCode jc = ConnectFail_ClassifyJoin(&ev);
            SDL_Log("[direct_p2p] joiner bilateral punch failed (port_disagreement=%d "
                    "bad_token=%d) -> %s",
                    (int)s_work.stun.port_disagreement,
                    (int)s_work.stun.diag_punch_bad_token, ConnectFail_Code(jc));
            Stun_CloseSocket(&s_work.stun);
            set_fail(jc);
            return DIRECT_P2P_FAILED_BILATERAL;
        }

        /* 7) Bilateral punch succeeded. Writeback BEFORE the HANDOFF
         * publish (done by the join_thread_fn wrapper on our return) —
         * Tick reads s_work.peer_ip/peer_public_port in join_tick_handoff,
         * so the post-punch translated endpoint must be visible by the
         * time HANDOFF is observed. */
        SDL_strlcpy(s_work.peer_ip, bp_peer_ip, sizeof(s_work.peer_ip));
        s_work.peer_public_port = bp_peer_port;

        if (s_work.peer_code[0] != '\0') {
            Config_SetString(CFG_KEY_NETPLAY_DIRECT_P2P_LAST_PEER_CODE, s_work.peer_code);
        }

        set_status("Connected!");
        return DIRECT_P2P_HANDOFF;
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

    /* Attempt is done; the wrapper publishes HANDOFF and Tick drives the
     * netplay.c handoff on the main thread. */
    set_status("Connected!");
    return DIRECT_P2P_HANDOFF;
}

/* Join worker: one join_attempt pass, plus the S2 auto-retry — on a
 * TERMINAL failure the joiner gets exactly ONE automatic full retry
 * with a FRESHLY BOUND local socket before the error is surfaced.
 * Rationale (docs/plan-netplay-connection.md §4): the first attempt can
 * die to transient causes a second pass fixes — the host was still in
 * its UPnP probe when our 2.5 s direct punch ran (start-skew), a first
 * packet lost while NAT mappings opened, or stuck conntrack/NAT state
 * pinned to the previous local port. join_attempt's failure paths all
 * close the attempt's socket, and each attempt re-runs STUN_DISCOVER
 * with local_port 0, so the retry naturally binds a fresh OS-assigned
 * port (dodging stale per-port NAT state).
 *
 * Non-retryable outcomes: DIRECT_P2P_IDLE (user cancel) and HANDOFF
 * (success). Everything terminal (FAILED_STUN / FAILED_SYMMETRIC /
 * FAILED_BILATERAL) retries once. */
static int SDLCALL join_thread_fn(void* data) {
    (void)data;

    /* See host_thread_fn for the rationale on this pre-NET_Init delay. */
    SDL_Delay(200);

    /* Idempotent NET_Init — Stun_Discover assumes we've called it. */
    NET_Init();

    DirectP2PState outcome = join_attempt();
    if (outcome != DIRECT_P2P_HANDOFF && outcome != DIRECT_P2P_IDLE) {
        SDL_Log("[direct_p2p] join attempt failed (state %d) — one automatic retry "
                "with a freshly bound socket", (int)outcome);
        set_status("Retrying...");
        outcome = join_attempt();
        if (outcome != DIRECT_P2P_HANDOFF && outcome != DIRECT_P2P_IDLE) {
            SDL_Log("[direct_p2p] join retry failed too (state %d) — surfacing", (int)outcome);
        }
    }
    set_state(outcome);
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
    s_drift_pending_valid = false; /* M2: drop any half-confirmed drift */
    s_host_deliver_seen = false;   /* S3 */
    s_host_waiting_since_ms = 0;   /* S3 */
    s_host_waiting_last_note_ms = 0;
    s_host_advisory_code = CONNECT_FAIL_NONE;
    s_host_unauth_drops = 0; /* S4a */
    s_host_challenge_seen = false; /* S4c */
    host_punch_gate_reset(); /* S4-review HIGH-1b */
    signal_cookie_reset();         /* S4c */
    SDL_SetAtomicInt(&s_host_registering, 0);
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
    s_drift_pending_valid = false; /* M2: drop any half-confirmed drift */
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

/* Cancel and JOIN the rendezvous re-REGISTER worker. Safe to call when
 * no worker is running. Callers MUST do this before mutating any s_work
 * field the worker's session-key derivation reads (public_ip /
 * advertised_port / nonce). */
static void host_rendezvous_stop(void) {
    if (s_rendezvous_thread != NULL) {
        SDL_SetAtomicInt(&s_rendezvous_cancel, 1);
        SDL_WaitThread(s_rendezvous_thread, NULL);
        s_rendezvous_thread = NULL;
    }
}

/* Respawn the rendezvous worker under whatever session key s_work now
 * implies, honoring the same kill switch as the original spawn in
 * host_thread_fn step 5. `why` names the caller in the failure log. */
static void host_rendezvous_restart(const char* why) {
    if (Config_GetBool(CFG_KEY_NETPLAY_DIRECT_P2P_DISABLE_BILATERAL)) {
        return;
    }
    SDL_SetAtomicInt(&s_rendezvous_cancel, 0);
    s_rendezvous_thread = SDL_CreateThread(host_rendezvous_thread_fn,
                                           "DirectP2PRendezvous", NULL);
    if (s_rendezvous_thread == NULL) {
        SDL_Log("[direct_p2p] WARNING: failed to respawn rendezvous thread "
                "%s; fallback disabled this session", why);
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
 * menu screen.
 *
 * Review M2: commits are DEBOUNCED — a drift must be confirmed by two
 * consecutive keepalives reporting the same new endpoint before the
 * code is rewritten (see s_drift_pending_* for the rationale and the
 * rebind-every-interval no-churn property). */
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
        s_drift_pending_valid = false; /* stable again — drop any candidate */
        return; /* mapping stable — keepalive did its NAT-refresh job */
    }

    /* Review M2 debounce: require a second consecutive keepalive to
     * confirm the SAME new endpoint before rewriting the displayed
     * code. First sighting (or a candidate that keeps changing — the
     * rebind-every-interval NAT) only records/replaces the candidate. */
    if (!s_drift_pending_valid || port != s_drift_pending_port ||
        !direct_p2p_ip_eq_normalized(ip, s_drift_pending_ip)) {
        SDL_strlcpy(s_drift_pending_ip, ip, sizeof(s_drift_pending_ip));
        s_drift_pending_port = port;
        s_drift_pending_valid = true;
        SDL_Log("[direct_p2p] STUN rebind drift CANDIDATE %s:%u (current %s:%u) — "
                "awaiting confirmation on the next keepalive",
                ip, (unsigned)port,
                s_work.stun.public_ip, (unsigned)s_work.stun.public_port);
        return;
    }
    s_drift_pending_valid = false;

    SDL_Log("[direct_p2p] STUN rebind drift CONFIRMED: public endpoint changed "
            "%s:%u -> %s:%u — displayed room code is stale",
            s_work.stun.public_ip, (unsigned)s_work.stun.public_port,
            ip, (unsigned)port);

    /* Stop the rendezvous re-REGISTER loop before mutating the fields it
     * reads (public_ip / advertised_port). */
    host_rendezvous_stop();

    SDL_strlcpy(s_work.stun.public_ip, ip, sizeof(s_work.stun.public_ip));
    s_work.stun.public_port = port;

    /* Recompute the advertised tuple. A live UPnP mapping pins the
     * advertised PORT (the router forwards it regardless of the STUN
     * mapping), so only the IP component can drift there; without UPnP
     * both components track the STUN-observed endpoint. */
    uint16_t new_adv_port = s_upnp_mapping.active ? s_upnp_mapping.external_port : port;
    uint32_t ip_be = ipv4_str_to_be(ip);
    char new_code[ROOM_CODE_BUF_LEN];
    /* S4b: the nonce stays STABLE across a drift re-encode — the
     * endpoint change already yields a new code/key/token, and the
     * nonce's job (unguessable derivations) is done either way. */
    if (ip_be != 0 && RoomCode_Encode(ip_be, new_adv_port, s_work.nonce, new_code) &&
        strcmp(new_code, s_work.host_code) != 0) {
        char old_redacted[ROOM_CODE_BUF_LEN];
        char new_redacted[ROOM_CODE_BUF_LEN];
        RoomCode_Redact(s_work.host_code, old_redacted);
        RoomCode_Redact(new_code, new_redacted);
        SDL_Log("[direct_p2p] room code re-encoded: %s -> %s (nonce redacted)",
                old_redacted, new_redacted);
        SDL_strlcpy(s_work.host_code, new_code, sizeof(s_work.host_code));
        set_status("Network changed! Share the NEW code.");
    }
    s_work.advertised_port = new_adv_port;

    /* S4a: the punch token is derived from the advertised tuple, so a
     * drift-committed endpoint means a NEW token (the re-encoded code
     * the user shares carries the new payload; the joiner derives from
     * that). Fail closed on derivation failure — better to ignore
     * punches than to accept unauthenticated ones. */
    s_work.punch_token_valid =
        Rendezvous_DerivePunchToken(ip_be, new_adv_port, s_work.nonce, s_work.punch_token);
    if (!s_work.punch_token_valid) {
        SDL_Log("[direct_p2p] WARNING: punch-token re-derivation failed after "
                "endpoint drift — punches will be ignored until re-host");
    }

    /* Respawn the rendezvous loop under the new session key (same kill
     * switch as the original spawn in host_thread_fn step 5). */
    host_rendezvous_restart("after endpoint drift");
}

/* --- S4-review HIGH-1b: room-code re-roll ------------------------------
 *
 * Draw a FRESH nonce, re-encode the room code, re-derive the punch
 * token, and tell the user. Invoked once the punch gate has absorbed
 * HOST_PUNCH_TOTAL_REROLL bad-token punches this session: whatever the
 * sender was searching for is thereby invalidated, and every source mute
 * is cleared so a legitimate peer that merely had a stale code gets a
 * clean slate along with the new code.
 *
 * Same threading discipline as the drift handler above — the rendezvous
 * worker derives its session key from (public_ip, advertised_port,
 * nonce), so it is cancelled and JOINED before s_work.nonce moves, then
 * respawned under the new key. Main thread only.
 *
 * Fails CLOSED and CHANGES NOTHING if either the CSPRNG or the
 * derivation is unavailable: keeping a working old code beats
 * advertising a code whose token we could not derive (which would
 * ignore every punch, including the real joiner's). */
static void host_reroll_room_code(void) {
    const uint32_t ip_be = ipv4_str_to_be(s_work.stun.public_ip);
    if (ip_be == 0) {
        return;
    }

    uint32_t new_nonce = 0;
    if (!RoomCode_GenerateNonce(&new_nonce)) {
        SDL_Log("[direct_p2p] punch-gate re-roll SKIPPED: CSPRNG unavailable — "
                "keeping the current room code (a predictable nonce would be "
                "worse than the flood)");
        /* Do not retry on every datagram: park the counter just under the
         * threshold so the next bad punch re-arms rather than spinning. */
        s_host_punch_bad_total = HOST_PUNCH_TOTAL_REROLL - 1;
        return;
    }

    char new_code[ROOM_CODE_BUF_LEN];
    uint8_t new_token[STUN_PUNCH_TOKEN_LEN];
    if (!RoomCode_Encode(ip_be, s_work.advertised_port, new_nonce, new_code) ||
        !Rendezvous_DerivePunchToken(ip_be, s_work.advertised_port, new_nonce,
                                     new_token)) {
        SDL_Log("[direct_p2p] punch-gate re-roll SKIPPED: re-encode/derive "
                "failed — keeping the current room code");
        s_host_punch_bad_total = HOST_PUNCH_TOTAL_REROLL - 1;
        return;
    }

    host_rendezvous_stop();

    s_work.nonce = new_nonce;
    SDL_strlcpy(s_work.host_code, new_code, sizeof(s_work.host_code));
    memcpy(s_work.punch_token, new_token, sizeof(s_work.punch_token));
    s_work.punch_token_valid = true;

    s_host_punch_rerolls++;
    s_host_punch_bad_total = 0;
    host_punch_gate_clear_mutes();

    SDL_Log("[direct_p2p] %s punch-gate RE-ROLL #%d/%d after %d bad-token "
            "punches — room code regenerated (every source mute cleared)",
            ConnectFail_Code(CONNECT_FAIL_PUNCH_AUTH),
            s_host_punch_rerolls, HOST_PUNCH_REROLL_MAX,
            HOST_PUNCH_TOTAL_REROLL);

    /* The user is staring at a code they may already have read out. Say
     * so explicitly on the overlay status line — same mechanism and the
     * same wording shape as the STUN-drift re-encode above. */
    set_status("Code was being probed. Share the NEW code.");

    host_rendezvous_restart("after a punch-gate re-roll");
}

/*
 * DELIVER handler — invoked from host_tick_receive after the magic-byte
 * gate. Parses the packet against our session key (derived from our own
 * public endpoint, same as the rendezvous thread). On a valid DELIVER
 * with a non-zero peer endpoint, transitions out of HOST_WAITING per
 * the bilateral fallback flow:
 *   - Self-DELIVER (peer public IP == our public IP) => ignore and stay
 *     HOST_WAITING with the resender alive (review H1 — it is our own
 *     stale registration or a spoof; a legitimate joiner hairpin-bails
 *     client-side before ever REGISTERing).
 *   - LAN peer => stay HOST_WAITING (we should have seen a direct punch
 *     already; rendezvous via public infra is the wrong path).
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
    if (!Rendezvous_DeriveSessionKey(ip_be, s_work.advertised_port, s_work.nonce,
                                     session_key)) {
        SDL_Log("[direct_p2p] DELIVER drop: failed to derive session key");
        return true;
    }

    char peer_ip[64] = { 0 };
    uint16_t peer_port = 0;
    const RendezvousDeliverResult dr =
        Rendezvous_ParseDeliverEx(pkt, len, session_key, peer_ip, &peer_port);
    if (dr != REND_DELIVER_MALFORMED) {
        /* S3 cause-8 evidence: the server answers EVERY REGISTER with a
         * DELIVER (zero-sentinel while unpaired), so seeing ANY valid
         * DELIVER proves the rendezvous path is alive. Consumed by the
         * host-waiting advisory in Tick. Main thread — no atomics. */
        s_host_deliver_seen = true;
    }
    if (dr != REND_DELIVER_PEER) {
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
        set_fail_msg(CONNECT_FAIL_INTERNAL, "Connection failed. Try again.");
        set_state(DIRECT_P2P_FAILED_BILATERAL);
    }
    return true;
}

/* S4c: server CHALLENGE handler — invoked from host_tick_receive's
 * rendezvous arm (main thread) when the '3SXR' frame's type byte is
 * CHALLENGE. Parses the cookie against our session key, publishes it
 * for the rendezvous worker's periodic resends, and IMMEDIATELY sends
 * one cookie'd REGISTER back to the frame's source (the signal server)
 * so slot binding costs one RTT instead of waiting out the worker's
 * next 5 s cadence. Main thread owns the socket in HOST_WAITING, so
 * the direct send is race-free. */
static void host_handle_challenge(const uint8_t* pkt, int len,
                                  NET_Address* src_addr, uint16_t src_port) {
    if (get_state() != DIRECT_P2P_HOST_WAITING) {
        return; /* stale challenge after a transition — ignore */
    }
    uint32_t ip_be = ipv4_str_to_be(s_work.stun.public_ip);
    uint8_t session_key[16];
    if (!Rendezvous_DeriveSessionKey(ip_be, s_work.advertised_port, s_work.nonce,
                                     session_key)) {
        return;
    }
    uint8_t cookie[REND_COOKIE_LEN];
    if (!Rendezvous_ParseChallenge(pkt, len, session_key, cookie)) {
        SDL_Log("[direct_p2p] CHALLENGE drop: bad frame or wrong session key");
        return;
    }
    s_host_challenge_seen = true;
    signal_cookie_publish(cookie);
    uint8_t register_pkt[REND_REGISTER_PKT_LEN];
    if (Rendezvous_BuildRegister(s_work.stun.public_port, session_key, cookie,
                                 register_pkt) &&
        s_work.stun.socket != NULL && src_addr != NULL) {
        /* Routed through the RENDEZVOUS_SEND seam (not a direct
         * Rendezvous_Send) so test_bilateral_punch.c can OBSERVE the
         * cookied echo: its `my_public_port` field must carry
         * s_work.stun.public_port (the source port the server checks
         * against rinfo.port) and NOT s_work.advertised_port, which
         * differs whenever UPnP maps an external port the STUN probe
         * never saw. Production (no -DNETPLAY_TEST_HOOKS) expands to the
         * identical direct Rendezvous_Send call. */
        (void)RENDEZVOUS_SEND(s_work.stun.socket, src_addr, src_port,
                              register_pkt, sizeof(register_pkt));
    }
    SDL_Log("[direct_p2p] rendezvous CHALLENGE answered (cookie echoed to %s:%u)",
            src_addr != NULL ? NET_GetAddressString(src_addr) : "?",
            (unsigned)src_port);
}

/* Host-side per-frame drain. Returns true once a valid inbound datagram
 * has been received and the handoff was executed. The first ACCEPTED
 * inbound packet on a direct-P2P Host socket is the joiner's
 * authenticated Stun_HolePunch probe ("3SX_PUNCH" + the 8-byte
 * code-derived token, S4a). We echo the validated payload back to the
 * source so the joiner's Stun_HolePunch loop sees an authenticated
 * response and returns success — otherwise the joiner would time out
 * and flag FAILED_SYMMETRIC even though connectivity is established.
 * Anything that fails the datagram gate is dropped WITHOUT consuming
 * the peer slot (see classify_host_datagram). */
static bool host_tick_receive(void) {
    if (s_work.stun.socket == NULL) {
        return false;
    }
    NET_Datagram* dgram = NULL;
    if (!NET_ReceiveDatagram(s_work.stun.socket, &dgram) || dgram == NULL) {
        return false;
    }
    /* S4a: one routing decision per datagram (classify_host_datagram).
     * Only an AUTHENTICATED punch — "3SX_PUNCH" + the 8-byte token both
     * sides derive from the room-code payload — may be accepted as the
     * peer. Pre-S4a, ANY non-'3SXR'/non-STUN datagram was captured as
     * the peer and echoed: session hijack for anyone who saw/guessed
     * the advertised endpoint, and permanent slot loss to a single
     * stray packet (the real joiner then failed with a misleading
     * "opponent build may be too old" after the MIST silence window). */
    switch (classify_host_datagram(dgram->buf, dgram->buflen,
                                   s_work.punch_token, s_work.punch_token_valid)) {
    case DP2P_HOST_DGRAM_RENDEZVOUS:
        /* Server->host frame ('3SXR'). Hosts only ever receive DELIVER
         * or (S4c) CHALLENGE; REGISTER/POLL are client->server.
         * Dispatch on the type byte — DELIVER pairs, CHALLENGE arms the
         * return-routability cookie. */
        if (dgram->buflen >= 6 && dgram->buf[5] == 4 /* REND_TYPE_CHALLENGE */) {
            host_handle_challenge(dgram->buf, dgram->buflen,
                                  dgram->addr, dgram->port);
        } else {
            try_handle_deliver(dgram->buf, dgram->buflen);
        }
        NET_DestroyDatagram(dgram);
        return true;

    case DP2P_HOST_DGRAM_STUN:
        /* S1: keepalive replies / late Stun_Discover duplicates route
         * to the drift detector, never to the peer capture. */
        host_handle_stun_rebind(dgram->buf, dgram->buflen);
        NET_DestroyDatagram(dgram);
        return false; /* not a peer — keep waiting */

    case DP2P_HOST_DGRAM_IGNORE: {
        /* Unauthenticated / unrecognized datagram: drop it, do NOT
         * echo, and KEEP WAITING — the peer slot is not consumed.
         * Rate-limited advisory so a scan/legacy-build burst is visible
         * in the log without flooding it. A punch-shaped payload with a
         * wrong token is called out as probable version mismatch. */
        const bool punch_shaped =
            Stun_HasPunchPrefix(dgram->buf, dgram->buflen);
        char src_ip[64];
        SDL_strlcpy(src_ip, NET_GetAddressString(dgram->addr), sizeof(src_ip));

        s_host_unauth_drops++;
        if (s_host_unauth_drops == 1 || (s_host_unauth_drops % 50) == 0) {
            SDL_Log("[direct_p2p] ADVISORY code=%s ignored unauthenticated datagram "
                    "#%d from %s:%u (len=%d%s) — still waiting for an authenticated "
                    "punch",
                    ConnectFail_Code(CONNECT_FAIL_PUNCH_AUTH),
                    s_host_unauth_drops, src_ip, (unsigned)dgram->port,
                    dgram->buflen,
                    punch_shaped
                        ? ", punch-shaped: peer build too old or wrong code"
                        : "");
        }

        /* S4-review HIGH-1b: only a punch-SHAPED datagram that failed
         * the token check is a gate probe. Arbitrary garbage (port
         * scans, stray traffic) is logged above but deliberately NOT
         * charged — it teaches an attacker nothing and charging it
         * would let unrelated noise trip the re-roll. */
        bool reroll_owed = false;
        if (punch_shaped) {
            reroll_owed = host_punch_gate_note_bad(src_ip, SDL_GetTicks());
        }
        NET_DestroyDatagram(dgram);
        if (reroll_owed) {
            host_reroll_room_code();
        }
        return false; /* keep waiting */
    }

    case DP2P_HOST_DGRAM_PEER_PUNCH:
        /* S4-review HIGH-1b: the token compare has ALREADY run and
         * passed, but if this source has been muted for grinding the
         * gate we refuse to ANSWER. Dropping here (rather than skipping
         * the classify) is what closes the oracle: the sender gets no
         * echo and no handoff, so a correct guess is indistinguishable
         * from a wrong one. Infrastructure arms ('3SXR', STUN) are
         * untouched by the mute. */
        if (host_punch_gate_is_muted(NET_GetAddressString(dgram->addr),
                                     SDL_GetTicks())) {
            s_host_punch_muted_drops++;
            if (s_host_punch_muted_drops == 1 ||
                (s_host_punch_muted_drops % 50) == 0) {
                SDL_Log("[direct_p2p] %s punch-gate suppressed an AUTHENTICATED "
                        "punch from muted source %s:%u (#%d) — that address "
                        "burned its budget guessing; it recovers when the mute "
                        "expires or the code re-rolls",
                        ConnectFail_Code(CONNECT_FAIL_PUNCH_AUTH),
                        NET_GetAddressString(dgram->addr),
                        (unsigned)dgram->port, s_host_punch_muted_drops);
            }
            NET_DestroyDatagram(dgram);
            return false; /* keep waiting */
        }
        break; /* authenticated peer — fall through to the capture below */
    }

    /* Capture the source endpoint — this is who we'll talk to. The
     * socket's internal "connected peer" state gets set when
     * configure_gekko wraps it into SDLNetAdapter; for the orchestrator
     * the source-IP:port from the first packet is the peer's public
     * endpoint (post-NAT translation, which is what we need for sends). */
    char src_ip[64];
    SDL_strlcpy(src_ip, NET_GetAddressString(dgram->addr), sizeof(src_ip));
    uint16_t src_port = dgram->port;

    /* Echo the (already-validated) punch payload back — Stun_HolePunch
     * on the joiner requires the byte-identical authenticated payload
     * from the expected peer, so echoing what we just validated
     * authenticates US to the joiner in the same exchange. */
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
    s_host_stun_retry_count = 0; /* S2: fresh host STUN-retry budget */
    s_host_stun_retry_at_ms = 0;
    SDL_SetAtomicInt(&s_q_head, 0);
    SDL_SetAtomicInt(&s_q_tail, 0);
    SDL_SetAtomicInt(&s_q_drops, 0);
    SDL_SetAtomicInt(&s_q_drops_logged, 0);
    s_outcome_reported = false;  /* S3 */
    s_host_deliver_seen = false; /* S3 */
    s_host_waiting_since_ms = 0; /* S3 */
    s_host_waiting_last_note_ms = 0;
    s_host_advisory_code = CONNECT_FAIL_NONE;
    s_host_unauth_drops = 0; /* S4a */
    s_host_challenge_seen = false; /* S4c */
    host_punch_gate_reset(); /* S4-review HIGH-1b */
    signal_cookie_reset();         /* S4c */
    SDL_SetAtomicInt(&s_host_registering, 0);
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
    s_host_stun_retry_count = 0; /* S2: fresh host STUN-retry budget */
    s_host_stun_retry_at_ms = 0;
    SDL_SetAtomicInt(&s_q_drops, 0);
    SDL_SetAtomicInt(&s_q_drops_logged, 0);
    /* R-1: a reject latched during a session whose teardown never ran the
     * callback (e.g. LAN CLI session with no orchestrator) must not leak
     * into this fresh session's teardown. */
    s_handshake_reject_latched = false;
    s_outcome_reported = false;  /* S3: fresh report per hosting session */
    s_host_deliver_seen = false; /* S3 */
    s_host_waiting_since_ms = 0; /* S3 */
    s_host_waiting_last_note_ms = 0;
    s_host_advisory_code = CONNECT_FAIL_NONE;
    s_host_unauth_drops = 0; /* S4a */
    s_host_challenge_seen = false; /* S4c */
    host_punch_gate_reset(); /* S4-review HIGH-1b */
    signal_cookie_reset();         /* S4c */
    SDL_SetAtomicInt(&s_host_registering, 0);
    Stun_ReleaseServerAddr(&s_work.stun); /* belt-and-braces before memset */
    s_stun_keepalive_last_ms = 0;
    s_rebind_txid_valid = false;
    s_drift_pending_valid = false; /* M2: drop any half-confirmed drift */
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
        set_fail_msg(CONNECT_FAIL_INTERNAL, "Connection failed. Try again.");
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
     * surface immediately, not after a STUN round-trip. (S3: safe to
     * memset s_work here — state == IDLE guarantees any previous worker
     * has published its terminal state, which happens-after its last
     * s_work write.)
     *
     * S4b: the decode result is a tri-state-plus — a legacy-checksum-
     * valid 11-char v1 or 14-char v2 code, and an unknown-version
     * 18-char code, each get their own explanation
     * (CONNECT_FAIL_CODE_VERSION_OLDER / _NEWER — L-4 split so log
     * triage can tell WHICH side is stale) instead of the
     * mysterious generic "Invalid room code.". */
    uint32_t ip_be = 0;
    uint16_t pub_port = 0;
    uint32_t code_nonce = 0;
    const RoomCodeDecodeResult dec =
        RoomCode_Decode(peer_code, &ip_be, &pub_port, &code_nonce);
    if (dec != ROOM_CODE_OK) {
        /* Review L-1: same belt-and-braces release the normal path does
         * before ITS memset — a ref'd STUN server address left in
         * s_work.stun by an earlier session must not be leaked by the
         * zeroing below. */
        Stun_ReleaseServerAddr(&s_work.stun);
        memset(&s_work, 0, sizeof(s_work));
        s_work.role = ROLE_JOIN;
        s_outcome_reported = false;
        switch (dec) {
        case ROOM_CODE_OLD_FORMAT:
            SDL_Log("[direct_p2p] room code is a valid pre-v3 (v1/v2) code — "
                    "the host runs an older build");
            set_fail_msg(CONNECT_FAIL_CODE_VERSION_OLDER,
                         "Code is from an older game version.");
            break;
        case ROOM_CODE_FUTURE_VERSION:
            SDL_Log("[direct_p2p] room code carries an unknown format-version "
                    "char — created by a newer build?");
            set_fail_msg(CONNECT_FAIL_CODE_VERSION_NEWER,
                         "Code is from a newer game version.");
            break;
        default:
            set_fail(CONNECT_FAIL_INVALID_CODE);
            break;
        }
        set_state(DIRECT_P2P_FAILED_PUNCH);
        return;
    }

    struct in_addr in;
    in.s_addr = ip_be;
    char peer_ip[64] = { 0 };
    if (inet_ntop(AF_INET, &in, peer_ip, sizeof(peer_ip)) == NULL) {
        Stun_ReleaseServerAddr(&s_work.stun); /* L-1: see decode path above */
        memset(&s_work, 0, sizeof(s_work));
        s_work.role = ROLE_JOIN;
        s_outcome_reported = false;
        set_fail(CONNECT_FAIL_INVALID_CODE);
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
    s_host_stun_retry_count = 0; /* S2: fresh host STUN-retry budget */
    s_host_stun_retry_at_ms = 0;
    SDL_SetAtomicInt(&s_q_drops, 0);
    SDL_SetAtomicInt(&s_q_drops_logged, 0);
    s_handshake_reject_latched = false; /* R-1: see BeginHost */
    s_outcome_reported = false;  /* S3: fresh report per join session */
    s_host_deliver_seen = false; /* S3 */
    s_host_waiting_since_ms = 0; /* S3 */
    s_host_waiting_last_note_ms = 0;
    s_host_advisory_code = CONNECT_FAIL_NONE;
    s_host_unauth_drops = 0; /* S4a */
    s_host_challenge_seen = false; /* S4c */
    host_punch_gate_reset(); /* S4-review HIGH-1b */
    signal_cookie_reset();         /* S4c */
    SDL_SetAtomicInt(&s_host_registering, 0);
    Stun_ReleaseServerAddr(&s_work.stun); /* belt-and-braces before memset */
    s_stun_keepalive_last_ms = 0;
    s_rebind_txid_valid = false;
    s_drift_pending_valid = false; /* M2: drop any half-confirmed drift */
    memset(&s_work, 0, sizeof(s_work));
    s_work.role = ROLE_JOIN;
    SDL_strlcpy(s_work.peer_code, peer_code, sizeof(s_work.peer_code));
    SDL_strlcpy(s_work.peer_ip, peer_ip, sizeof(s_work.peer_ip));
    s_work.peer_public_port = pub_port;
    s_work.nonce = code_nonce; /* S4b: feeds session-key + punch-token derivation */

    s_thread = SDL_CreateThread(join_thread_fn, "DirectP2PJoin", NULL);
    if (s_thread == NULL) {
        set_fail_msg(CONNECT_FAIL_INTERNAL, "Connection failed. Try again.");
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
     * longest cancel-uninterruptible span among the active workers:
     * Stun_Discover takes no cancel flag, so a worker inside it blocks
     * us for up to stun_budget_ms() — the user-configurable STUN
     * timeout, clamped to [1000, 15000] ms (default 4000) precisely so
     * this join stays bounded (review L-4). Stun_HolePunch checks its
     * cancel flag every ~10-50 ms. */
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
    s_drift_pending_valid = false; /* M2: drop any half-confirmed drift */
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
    s_host_stun_retry_count = 0; /* S2: drop any pending host STUN retry */
    s_host_stun_retry_at_ms = 0;
    s_outcome_reported = false;  /* S3 */
    s_host_deliver_seen = false; /* S3 */
    s_host_waiting_since_ms = 0; /* S3 */
    s_host_waiting_last_note_ms = 0;
    s_host_advisory_code = CONNECT_FAIL_NONE;
    s_host_unauth_drops = 0; /* S4a */
    s_host_challenge_seen = false; /* S4c */
    host_punch_gate_reset(); /* S4-review HIGH-1b */
    signal_cookie_reset();         /* S4c */
    SDL_SetAtomicInt(&s_host_registering, 0);
    set_state(DIRECT_P2P_IDLE);
}

/* S3 — see direct_p2p.h. Latch a post-handoff session failure with its
 * taxonomy code: status text for the overlay now, FAILED_HANDSHAKE park
 * at teardown, and one machine-coded report line from Tick's reporting
 * path once the terminal state is visible. */
void DirectP2P_NotifySessionFailed(ConnectFailCode code, const char* reason) {
    set_status((reason != NULL && reason[0] != '\0')
                   ? reason
                   : ConnectFail_UserText(code));
    s_work.fail_code = code;
    s_handshake_reject_latched = true;
    /* The pre-handoff outcome (OK) may already have been reported; this
     * is a NEW outcome for the same attempt-set — re-arm the reporter. */
    s_outcome_reported = false;
    SDL_Log("[direct_p2p] session failed post-handoff (%s): %s",
            ConnectFail_Code(code), reason ? reason : "(no reason)");
}

/* See direct_p2p.h — arm-time refusal with no session to tear down:
 * publish the reason and park in FAILED_HANDSHAKE immediately so the
 * overlay renders ERROR + reason on the very next frame.
 *
 * Rebase integration note: this predates the S3 taxonomy, which made
 * Tick's terminal FAILED_HANDSHAKE case emit one machine-coded
 * "[netplay-connect] FAIL code=..." line via report_connect_outcome.
 * Parking the state WITHOUT a fail_code would emit that line with the
 * zero code (P2P_FAIL_NONE) and role=none — a dishonest log entry for a
 * fully-attributable cause. Stamp the taxonomy explicitly, and re-arm
 * the reporter for the same reason NotifySessionFailed does. */
void DirectP2P_RefuseSession(const char* reason) {
    set_status((reason != NULL && reason[0] != '\0') ? reason : "Netplay unavailable.");
    s_work.fail_code = CONNECT_FAIL_BALANCE_UNAVAILABLE;
    s_outcome_reported = false;
    SDL_Log("[direct_p2p] refusing session: %s", reason ? reason : "(no reason)");
    set_state(DIRECT_P2P_FAILED_HANDSHAKE);
}

/* R-1 — see direct_p2p.h. Records the reject reason for the overlay and
 * latches the teardown redirect to FAILED_HANDSHAKE. */
void DirectP2P_NotifySessionRejected(const char* reason) {
    DirectP2P_NotifySessionFailed(CONNECT_FAIL_PEER_REJECTED, reason);
}

/* S3 Part A(3): per-frame informative pass for HOST_WAITING (see the
 * s_host_waiting_* comment). Called from Tick's HOST_WAITING branch —
 * main thread, like the keepalive tick beside it. */
static void host_waiting_tick(void) {
    const uint64_t now = SDL_GetTicks();
    if (s_host_waiting_since_ms == 0) {
        s_host_waiting_since_ms = now;
        s_host_waiting_last_note_ms = now;
        return;
    }
    uint32_t waited_ms = (uint32_t)(now - s_host_waiting_since_ms);
#ifdef NETPLAY_TEST_HOOKS
    /* S4-review MEDIUM-2 seam: CONNECT_HOST_ADVISORY_MS is a 30 s
     * compile-time constant, so the COOKIE_REJECTED host-advisory test
     * would otherwise have to sleep 30 s of real time. Scale the elapsed
     * clock instead of the threshold: the classifier under test still
     * sees the real CONNECT_HOST_ADVISORY_MS boundary, so the test
     * exercises the shipped constant rather than a test-only one. */
    if (s_host_advisory_scale > 1) {
        waited_ms *= (uint32_t)s_host_advisory_scale;
    }
#endif

    /* Cause-8 advisory (once per hosting session): the server answers
     * every REGISTER with a DELIVER, so zero DELIVERs after the
     * threshold means the rendezvous path is dead — and with no UPnP
     * mapping either, this room is likely unjoinable. Tell the host NOW
     * instead of letting a friend fail minutes later. */
    if (s_host_advisory_code == CONNECT_FAIL_NONE &&
        SDL_GetAtomicInt(&s_host_registering) != 0) {
        const ConnectFailCode adv = ConnectFail_ClassifyHostWaiting(
            s_upnp_mapping.active, s_host_deliver_seen,
            s_host_challenge_seen, waited_ms);
        if (adv != CONNECT_FAIL_NONE) {
            s_host_advisory_code = adv;
            char line[256];
            SDL_snprintf(line, sizeof(line),
                         "[netplay-connect] ADVISORY code=%s role=host waited_ms=%u "
                         "upnp=%d deliver_seen=0 challenge_seen=%d — %s",
                         ConnectFail_Code(adv), (unsigned)waited_ms,
                         (int)s_upnp_mapping.active,
                         (int)s_host_challenge_seen,
                         adv == CONNECT_FAIL_HOST_UNMAPPABLE
                             ? "no UPnP mapping and no rendezvous contact: joiners "
                               "will likely fail; ask the other side to host"
                         : adv == CONNECT_FAIL_COOKIE_REJECTED
                             ? "server challenges us but never accepts the cookie "
                               "echo: auth/version trouble, not a dead server"
                             : "rendezvous server unreachable: the bilateral "
                               "fallback is unavailable, direct joins still work");
            Netplay_LogConnectEvent(line);
            if (adv == CONNECT_FAIL_HOST_UNMAPPABLE ||
                adv == CONNECT_FAIL_COOKIE_REJECTED) {
                /* Overlay line 3 — the room code stays displayed (a
                 * direct cone-NAT join could still land), but the host
                 * is no longer silently unaware. */
                set_status(ConnectFail_UserText(adv));
            }
        }
    }

    /* Minute-cadence liveness note: log + on-screen elapsed counter (the
     * unmappable / cookie advisories own the status line when present). */
    if (now - s_host_waiting_last_note_ms >= 60000u) {
        s_host_waiting_last_note_ms = now;
        const unsigned min = waited_ms / 60000u;
        SDL_Log("[direct_p2p] host still advertising (%u min): rendezvous=%s upnp=%s",
                min, s_host_deliver_seen ? "alive" : "SILENT",
                s_upnp_mapping.active ? "mapped" : "none");
        if (s_host_advisory_code != CONNECT_FAIL_HOST_UNMAPPABLE &&
            s_host_advisory_code != CONNECT_FAIL_COOKIE_REJECTED) {
            char st[64];
            SDL_snprintf(st, sizeof(st), "Waiting for player 2... (%u min)", min);
            set_status(st);
        }
    }
}

/* S3 — one attributed outcome line per attempt-set, written to the
 * per-session netplay log (lazily opened by Netplay_LogConnectEvent for
 * pre-session failures) + SDL_Log. Main-thread only: called exclusively
 * from Tick after it observes a terminal state, so every s_work field
 * the worker wrote before publishing that state is visible. */
static void report_connect_outcome(DirectP2PState st, bool success) {
    if (s_outcome_reported) {
        return;
    }
    s_outcome_reported = true;
    char line[512];
    if (success) {
        SDL_snprintf(line, sizeof(line),
                     "[netplay-connect] OK role=%s attempts=%d "
                     "t_ms upnp=%u stun=%u punch=%u signal=%u bilateral=%u "
                     "stun=%d/%d portdis=%d",
                     s_work.role == ROLE_HOST ? "host" : "join",
                     s_work.join_attempts,
                     s_work.t_upnp_ms, s_work.t_stun_ms, s_work.t_punch_ms,
                     s_work.t_signal_ms, s_work.t_bilateral_ms,
                     s_work.stun.diag_servers_answered,
                     s_work.stun.diag_servers_probed,
                     (int)s_work.stun.port_disagreement);
    } else {
        SDL_snprintf(line, sizeof(line),
                     "[netplay-connect] FAIL code=%s state=%d role=%s msg=\"%s\" "
                     "attempts=%d t_ms upnp=%u stun=%u punch=%u signal=%u bilateral=%u "
                     "stun=%d/%d sends_ok=%d dns_all_failed=%d portdis=%d "
                     "deliver=any:%d,real:%d",
                     ConnectFail_Code(s_work.fail_code),
                     (int)st,
                     s_work.role == ROLE_HOST ? "host"
                     : s_work.role == ROLE_JOIN ? "join" : "none",
                     s_status,
                     s_work.join_attempts,
                     s_work.t_upnp_ms, s_work.t_stun_ms, s_work.t_punch_ms,
                     s_work.t_signal_ms, s_work.t_bilateral_ms,
                     s_work.stun.diag_servers_answered,
                     s_work.stun.diag_servers_probed,
                     s_work.stun.diag_sends_ok,
                     (int)s_work.stun.diag_dns_all_failed,
                     (int)s_work.stun.port_disagreement,
                     (int)s_work.ev_deliver_any, (int)s_work.ev_deliver_real);
    }
    Netplay_LogConnectEvent(line);
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
        /* S3: elapsed-time status + cause-8 advisory (see host_waiting_tick). */
        host_waiting_tick();
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
        /* S3: one success line with the stage timings — the "how long
         * did each phase take in the field" data reports need.
         *
         * S3-review HIGH-2: once a post-handoff failure has been latched
         * (DirectP2P_NotifySessionFailed re-arms the reporter while the
         * orchestrator is still parked here — Netplay_Run runs BEFORE
         * DirectP2P_Tick in the same frame, and the netplay teardown that
         * publishes FAILED_HANDSHAKE only runs on the NEXT frame's
         * EXITING pass), this success report MUST NOT re-fire: it would
         * consume the re-arm as a second, spurious OK line and thereby
         * suppress the real FAIL line the FAILED_HANDSHAKE case emits.
         * The latch is main-thread, like every reader here. */
        if (!s_handshake_reject_latched) {
            report_connect_outcome(st, true);
        }
        return;

    case DIRECT_P2P_FAILED_STUN:
        /* S2: HOST auto-retry with backoff. FAILED_STUN on the host is
         * frequently transient (DHCP renew mid-discovery, DNS hiccup,
         * momentary uplink loss) and the host has nothing else to do —
         * parking terminal turns a 5-second blip into a dead room.
         * Bounded per hosting session; each retry re-runs the full
         * host_thread_fn. The UPnP step reuses a live mapping from the
         * previous attempt rather than re-probing (review L-5 — a
         * transient re-probe failure must not desync upnp_ok from
         * s_upnp_mapping.active); with no live mapping it probes
         * fresh. The JOINER already retried inside join_thread_fn and
         * stays terminal here. */
        if (s_work.role == ROLE_HOST && s_host_stun_retry_count < HOST_STUN_MAX_RETRIES) {
            uint64_t now = SDL_GetTicks();
            if (s_host_stun_retry_at_ms == 0) {
                s_host_stun_retry_at_ms = now + HOST_STUN_RETRY_BACKOFF_MS;
                SDL_Log("[direct_p2p] host STUN discovery failed — auto-retry %d/%d in %u ms",
                        s_host_stun_retry_count + 1, HOST_STUN_MAX_RETRIES,
                        (unsigned)HOST_STUN_RETRY_BACKOFF_MS);
                set_status("Connection failed. Retrying...");
                return;
            }
            if (now < s_host_stun_retry_at_ms) {
                return; /* backoff in progress */
            }
            s_host_stun_retry_at_ms = 0;
            s_host_stun_retry_count++;
            /* Reap the failed worker before re-spawning (it has exited —
             * it published FAILED_STUN as its last act). */
            if (s_thread != NULL) {
                SDL_WaitThread(s_thread, NULL);
                s_thread = NULL;
            }
            SDL_Log("[direct_p2p] host STUN auto-retry %d/%d starting",
                    s_host_stun_retry_count, HOST_STUN_MAX_RETRIES);
            set_status("Preparing...");
            /* Publish the intermediate state BEFORE spawning so a Tick
             * racing the worker's own set_state never re-enters this
             * case mid-spawn. host_thread_fn re-publishes UPNP_PROBE
             * itself after its startup delay. */
            set_state(DIRECT_P2P_UPNP_PROBE);
            s_thread = SDL_CreateThread(host_thread_fn, "DirectP2PHost", NULL);
            if (s_thread == NULL) {
                SDL_Log("[direct_p2p] host STUN auto-retry: thread spawn failed");
                set_fail_msg(CONNECT_FAIL_INTERNAL, "Connection failed. Try again.");
                set_state(DIRECT_P2P_FAILED_STUN);
            }
            return;
        }
        /* joiner, or retry budget exhausted — terminal */
        report_connect_outcome(st, false);
        return;

    case DIRECT_P2P_FAILED_SYMMETRIC:
    case DIRECT_P2P_FAILED_PUNCH:
        /* Terminal failure; no auto-retry. The status-rendering
         * overlay (Step 8) is responsible for showing the reason. The
         * caller (menu) will eventually issue DirectP2P_Cancel to
         * return to IDLE. */
        report_connect_outcome(st, false);
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
         * NOT publish HANDOFF (mirroring the direct-path convention in
         * host_tick_receive, where set_state(DIRECT_P2P_HANDOFF)
         * precedes the do_handoff call), so we set_state HANDOFF after.
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
                /* S3 causes 5-6 (host side): every punch ran against a
                 * real DELIVER'd endpoint; the NAT pair is the blocker. */
                {
                    ConnectJoinEvidence ev = { 0 };
                    ev.deliver_any = true;
                    ev.deliver_real = true;
                    ev.bilateral_punched = false;
                    ev.port_disagreement = s_work.stun.port_disagreement;
                    ev.punch_bad_token = s_work.stun.diag_punch_bad_token; /* S4a */
                    set_fail(ConnectFail_ClassifyJoin(&ev));
                }
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
        report_connect_outcome(st, false);
        return;

    case DIRECT_P2P_FAILED_HANDSHAKE:
        /* Terminal — post-handoff session failure (MIST reject or an S3
         * deadline). Overlay shows ERROR + the reason; DirectP2P_Cancel
         * (or the OSD retry's process re-exec) returns to IDLE. */
        report_connect_outcome(st, false);
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

bool DirectP2P_HostStunRetryPending(void) {
    /* S3-review M-3: true while Tick's FAILED_STUN case still owns the
     * outcome — a HOST parked in FAILED_STUN with S2 auto-retries left
     * (backoff pending or about to respawn). Once the retry budget is
     * spent (or for a joiner, whose retry ran inside its own worker)
     * FAILED_STUN is TERMINAL: Tick has emitted the attributed FAIL
     * report, and nav must take its exit (a) instead of waiting out the
     * 150 s deadline and logging a second, contradictory
     * P2P_FAIL_TIMEOUT_ORCHESTRATOR for the same failure. Main-thread
     * only, like the retry bookkeeping it reads. */
    return get_state() == DIRECT_P2P_FAILED_STUN &&
           s_work.role == ROLE_HOST &&
           s_host_stun_retry_count < HOST_STUN_MAX_RETRIES;
}

const char* DirectP2P_GetHostCode(void) {
    if (get_state() != DIRECT_P2P_HOST_WAITING) return "";
    return s_work.host_code;
}

const char* DirectP2P_GetStatusText(void) {
    return s_status;
}

#ifdef NETPLAY_TEST_HOOKS
/* S3-review HIGH-2: run the session-teardown callback exactly as
 * netplay.c's EXITING pass would (it is registered via
 * Netplay_SetSessionTeardownCallback in DirectP2P_Init). Lets the
 * harness drive the notify -> teardown -> FAILED_HANDSHAKE -> report
 * sequence without standing up a full GekkoNet session. */
void DirectP2P_TestHook_RunTeardown(void) {
    direct_p2p_on_teardown();
}

/* S4-review HIGH-1b: punch-gate throttle seams (see direct_p2p.h). */
void DirectP2P_TestHook_SetHostAdvisoryScale(int scale) {
    s_host_advisory_scale = (scale > 1) ? scale : 1;
}

void DirectP2P_TestHook_PunchGateReset(void) {
    host_punch_gate_reset();
}

bool DirectP2P_TestHook_PunchGateNoteBad(const char* src_ip, uint32_t now_ms) {
    return host_punch_gate_note_bad(src_ip, now_ms);
}

bool DirectP2P_TestHook_PunchGateIsMuted(const char* src_ip, uint32_t now_ms) {
    return host_punch_gate_is_muted(src_ip, now_ms);
}

void DirectP2P_TestHook_PunchGateClearMutes(void) {
    host_punch_gate_clear_mutes();
}

void DirectP2P_TestHook_PunchGateCounters(int* bad_total, int* rerolls) {
    if (bad_total) *bad_total = s_host_punch_bad_total;
    if (rerolls) *rerolls = s_host_punch_rerolls;
}

void DirectP2P_TestHook_PunchGateLimits(int* src_max_bad, uint32_t* mute_ms,
                                        int* total_reroll, int* reroll_max,
                                        int* src_table) {
    if (src_max_bad) *src_max_bad = HOST_PUNCH_SRC_MAX_BAD;
    if (mute_ms) *mute_ms = HOST_PUNCH_MUTE_MS;
    if (total_reroll) *total_reroll = HOST_PUNCH_TOTAL_REROLL;
    if (reroll_max) *reroll_max = HOST_PUNCH_REROLL_MAX;
    if (src_table) *src_table = HOST_PUNCH_SRC_TABLE;
}
#endif /* NETPLAY_TEST_HOOKS */

#endif /* ENABLE_NETPLAY */
