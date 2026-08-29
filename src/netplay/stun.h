#ifndef NETPLAY_STUN_H
#define NETPLAY_STUN_H

#include <SDL3/SDL_atomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Result of a STUN binding request
typedef struct {
    char public_ip[64];                // String representation (IPv4 or IPv6)
    uint16_t public_port;              // Host byte order
    uint16_t local_port;               // Host byte order — actual OS-bound port (may differ from public_port)
    struct NET_DatagramSocket* socket; // The socket used for STUN (reuse for hole punching)
    /* S1 host liveness (docs/plan-netplay-connection.md): the resolved
     * address of the STUN server that answered Stun_Discover, kept
     * ref'd so Stun_SendKeepalive can re-issue binding requests on the
     * same socket without doing DNS on the caller's thread. Owned by
     * this struct; released by Stun_CloseSocket or
     * Stun_ReleaseServerAddr. NULL when no discovery has succeeded. */
    struct NET_Address* server_addr;
    uint16_t server_port;              // Host byte order — port of server_addr
    /* S2 (docs/plan-netplay-connection.md §4): true when at least two
     * STUN servers answered with DIFFERENT mapped ports for the same
     * local socket — per-destination translation, i.e. a symmetric-NAT
     * signal. Recorded for S3 failure attribution; discovery itself
     * still succeeds with the FIRST responder's endpoint. */
    bool port_disagreement;

    /* S3 failure-attribution evidence (docs/plan-netplay-connection.md
     * §5). Filled by Stun_Discover on BOTH the success and the failure
     * path, so a false return still carries the counters
     * ConnectFail_ClassifyStunDiscover needs to distinguish "DNS dead /
     * no network" from "outbound UDP filtered". */
    int diag_servers_probed;   /* probe slots armed (resolved + fallbacks) */
    int diag_servers_answered; /* slots with a parseable Binding Response */
    int diag_sends_ok;         /* NET_SendDatagram calls that reported success */
    bool diag_dns_all_failed;  /* every configured hostname failed getaddrinfo */
    /* S3-review M-2: discovery failed LOCALLY before any probing (UDP
     * socket creation/bind failed — fd exhaustion, EADDRINUSE, ...).
     * With every other counter zero this case is indistinguishable from
     * "no network at all", and the all-zeros classifier fallback used to
     * misreport it as DNS_ALLDOWN ("No internet connection"). Callers
     * classify it as CONNECT_FAIL_INTERNAL instead. */
    bool diag_socket_fail;

    /* S4a punch-auth evidence: during Stun_HolePunch, a datagram arrived
     * from the EXPECTED peer IP carrying the "3SX_PUNCH" prefix but a
     * missing/short/wrong token — the signature of a peer running a
     * build with a different token derivation (older version) or a
     * mismatched room code. OR-accumulated across HolePunch calls (the
     * direct punch and the bilateral punch of one join attempt share
     * this StunResult); cleared by Stun_Discover's memset at the start
     * of each attempt. ConnectFail_ClassifyJoin upgrades a punch
     * failure carrying this bit to P2P_FAIL_PUNCH_AUTH. */
    bool diag_punch_bad_token;

    /* S4-review L-3: the platform CSPRNG was unavailable, so no STUN
     * transaction ID could be generated and discovery refused to send.
     * Reported as an INTERNAL failure, not as a connectivity one —
     * nothing ever left the machine. See build_binding_request. */
    bool diag_csprng_fail;
} StunResult;

/// Perform STUN Binding Requests (RFC 5389) against the built-in server
/// pool (Google, Cloudflare, Nextcloud). S2: all servers are probed IN
/// PARALLEL from the one socket with per-server retransmits at
/// 0/500/1500 ms (RFC 5389 §7.2.1, truncated to 3 sends — parallel
/// servers substitute for deeper retransmission); DNS for all servers
/// resolves concurrently on a side thread with a numeric-IP fallback
/// list so a dead resolver cannot consume the budget. First parseable
/// Binding Response wins and its server is retained in server_addr for
/// S1 keepalives. `timeout_ms` is the overall wall-clock budget
/// (<= 0 selects the built-in 4000 ms default); callers wire
/// CFG_KEY_NETPLAY_DIRECT_P2P_STUN_TIMEOUT_MS here.
/// Returns true on success and fills `result`.
/// The socket in result->socket is left open for hole punching.
bool Stun_Discover(StunResult* result, uint16_t local_port, int timeout_ms);

/// Close the STUN socket when done
void Stun_CloseSocket(StunResult* result);

/// Encode an IP string + port into an endpoint string.
/// out_code must be at least 64 bytes.
void Stun_EncodeEndpoint(const char* ip, uint16_t port, uint16_t local_port, char* out_code);

/// Decode an endpoint string back into IP string + port.
/// out_ip must be at least 64 bytes.
/// Returns true on success.
bool Stun_DecodeEndpoint(const char* code, char* out_ip, uint16_t* out_port, uint16_t* out_local_port);

/* --- S4a punch authentication ----------------------------------------- */

/* The hole-punch payload is "3SX_PUNCH" (9 bytes, no NUL) followed by
 * an 8-byte token derived from the room-code payload
 * (Rendezvous_DerivePunchToken). Both directions send and REQUIRE this
 * exact 17-byte payload: the joiner always validated the host's echo,
 * and since S4a the host validates the joiner's punch too — an
 * unauthenticated datagram is ignored and the host keeps waiting
 * instead of handing its one peer slot to the first stray packet. */
#define STUN_PUNCH_TOKEN_LEN 8
#define STUN_PUNCH_PREFIX_LEN 9 /* strlen("3SX_PUNCH") */
#define STUN_PUNCH_PAYLOAD_LEN (STUN_PUNCH_PREFIX_LEN + STUN_PUNCH_TOKEN_LEN)

/// Compose the 17-byte authenticated punch payload into `out`.
void Stun_BuildPunchPayload(const uint8_t token[STUN_PUNCH_TOKEN_LEN],
                            uint8_t out[STUN_PUNCH_PAYLOAD_LEN]);

/// True iff buf/len is EXACTLY the authenticated punch payload for
/// `token` (17 bytes, "3SX_PUNCH" prefix, constant-time token compare).
bool Stun_IsPunchPayload(const uint8_t* buf, int len,
                         const uint8_t token[STUN_PUNCH_TOKEN_LEN]);

/// True iff buf/len starts with the "3SX_PUNCH" prefix (any length /
/// token) — used to distinguish "peer speaks the punch protocol but
/// failed auth" (version mismatch evidence) from unrelated traffic.
bool Stun_HasPunchPrefix(const uint8_t* buf, int len);

/// Hole punches NAT to connect to peer. Blocks for up to `punch_duration_ms`.
/// `punch_token` (required, 8 bytes from Rendezvous_DerivePunchToken)
/// authenticates the exchange in BOTH directions: we send it with every
/// punch and only accept a datagram carrying it back.
// Updates `peer_ip` and `peer_port` with the true translated endpoint if successful.
///
/// S6: this is now a thin BLOCKING DRIVER over the non-blocking punch-leg
/// stepper below — same wire behaviour, same accept criteria, same
/// confirmation burst. It remains the API for callers that own the socket
/// exclusively for the whole window and have nothing to interleave.
bool Stun_HolePunch(StunResult* local, char* peer_ip, uint16_t* peer_port,
                    const uint8_t punch_token[STUN_PUNCH_TOKEN_LEN],
                    int punch_duration_ms, SDL_AtomicInt* cancel_flag);

/* --- S6 non-blocking punch leg (docs/plan-netplay-connection.md §8) ----
 *
 * The joiner used to run its three establishment loops SERIALLY, so a
 * failing join cost the SUM of their budgets. Racing them on the one
 * worker thread and the one socket needs a punch that does not own the
 * socket for its whole window: a leg emits its due datagrams when pumped
 * and is OFFERED whatever the shared receive loop reads. No new threads,
 * no new locks.
 *
 * Lifecycle: Begin -> (Pump + Offer)* -> End. `confirmed` latches on the
 * first authenticated datagram from the peer IP; after that the leg keeps
 * burst-sending to the CONFIRMED endpoint until ConfirmDone(), which is
 * the same ~600 ms tail Stun_HolePunch has always sent (a peer whose own
 * loop started late or lost packets must still see one of ours).
 */
typedef struct {
    struct NET_Address* target;      /* ref'd send target; NULL when inactive */
    uint16_t target_port;            /* host order; retargeted on accept */
    uint32_t start_ms;               /* Begin() timestamp — drives the cadence */
    uint32_t last_send_ms;
    bool     sent_any;
    bool     confirmed;
    uint32_t confirm_ms;             /* when `confirmed` latched */
    char     peer_ip[64];            /* observed peer IP once confirmed */
    uint8_t  msg[STUN_PUNCH_PAYLOAD_LEN];
    uint8_t  token[STUN_PUNCH_TOKEN_LEN];
} StunPunchLeg;

/// Arm a leg at `peer_ip:peer_port`. Resolves the address (numeric
/// dotted-quads resolve without blocking; a hostname is polled for up to
/// ~3 s exactly as Stun_HolePunch has always done). Returns false and
/// leaves the leg inactive on resolve failure.
bool Stun_PunchBegin(StunPunchLeg* leg, const char* peer_ip, uint16_t peer_port,
                     const uint8_t punch_token[STUN_PUNCH_TOKEN_LEN],
                     uint32_t now_ms);

/// Emit this leg's due datagram, if any. Non-blocking, never receives.
/// Cadence: 50 ms for the first 500 ms, then 200 ms (S2 adaptive cadence);
/// once confirmed, 50 ms for the confirmation tail.
void Stun_PunchPump(StunPunchLeg* leg, struct NET_DatagramSocket* sock, uint32_t now_ms);

/// Offer a datagram the shared receive loop just read. Returns true iff
/// this leg CONSUMED it (it was an authenticated punch from our peer IP).
/// A punch-shaped datagram from the peer IP with a bad token sets
/// `local->diag_punch_bad_token` and is consumed as well — it is ours, it
/// is just not acceptable. `local` may be NULL.
bool Stun_PunchOffer(StunPunchLeg* leg, StunResult* local,
                     const uint8_t* buf, int len,
                     const char* src_ip, uint16_t src_port, uint32_t now_ms);

/// True once the leg has confirmed AND finished its confirmation tail —
/// i.e. the caller may hand the endpoint off.
bool Stun_PunchSettled(const StunPunchLeg* leg, uint32_t now_ms);

/// Read back the confirmed endpoint. Only meaningful once `confirmed`.
void Stun_PunchEndpoint(const StunPunchLeg* leg, char* out_ip, int ip_buf_size,
                        uint16_t* out_port);

/// True iff the leg is armed.
bool Stun_PunchActive(const StunPunchLeg* leg);

/// True iff the leg has latched an authenticated punch from the peer.
bool Stun_PunchConfirmed(const StunPunchLeg* leg);

/// Release the leg's address ref and mark it inactive. Idempotent.
void Stun_PunchEnd(StunPunchLeg* leg);

/// The length of the post-confirmation burst, in ms (exposed so callers
/// and tests reason about the same number the implementation uses).
#define STUN_PUNCH_CONFIRM_MS 600

/* --- S1 host liveness (docs/plan-netplay-connection.md) --------------- */

/// Re-issue a STUN Binding Request on result->socket toward the server
/// that answered Stun_Discover (result->server_addr). Non-blocking: one
/// datagram send, no receive. Writes the fresh 12-byte transaction ID to
/// `out_txid` so the caller can match the asynchronous response (which
/// arrives on the shared socket and must be routed through
/// Stun_ParseBindingResponse by the socket's poller). Returns false when
/// no socket/server_addr is available or the send fails.
bool Stun_SendKeepalive(StunResult* result, uint8_t out_txid[12]);

/// Cheap classifier: true iff buf/len shapes like a STUN Binding Success
/// Response (>= 20 bytes, type 0x0101, RFC 5389 magic cookie). Used by
/// socket pollers to keep STUN traffic out of peer-datagram handling.
bool Stun_IsBindingResponse(const uint8_t* buf, int len);

/// Full parse of a Binding Success Response against the expected
/// transaction ID. On success writes the mapped public endpoint to
/// out_ip (dotted quad / IPv6 string, ip_buf_size >= 64 recommended)
/// and out_port (host order). Returns false on any mismatch.
bool Stun_ParseBindingResponse(const uint8_t* buf, int len, const uint8_t txid[12], char* out_ip, int ip_buf_size,
                               uint16_t* out_port);

/// Release only the ref'd STUN server address (e.g. after the socket has
/// been handed off to netplay and keepalives are no longer this
/// module's job). Safe on NULL/already-released.
void Stun_ReleaseServerAddr(StunResult* result);

#ifdef NETPLAY_TEST_HOOKS
/* S2 test seam: replace the Stun_Discover server pool with
 * caller-supplied entries (numeric-IP localhost mocks). While an
 * override is installed the numeric fallback list is NOT armed, so
 * tests control the exact endpoint set. The array must outlive the
 * override (tests use function-scope statics). Pass (NULL, 0) to
 * restore the production pool. */
typedef struct {
    const char* host; /* hostname or numeric dotted-quad */
    uint16_t port;
} StunServerDesc;

void Stun_TestHook_SetServers(const StunServerDesc* servers, int count);
#endif /* NETPLAY_TEST_HOOKS */

#ifdef __cplusplus
}
#endif

#endif
