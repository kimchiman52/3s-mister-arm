#ifndef NETPLAY_NATPMP_H
#define NETPLAY_NATPMP_H

/**
 * @file natpmp.h
 * @brief Hand-rolled PCP (RFC 6887) + NAT-PMP (RFC 6886) port-mapping
 *        client — the third mapping backend beside UPnP-IGD.
 *
 * S7 of docs/plan-netplay-connection.md. miniupnpc only speaks UPnP IGD;
 * many routers (notably Apple/BSD-derived firmware) speak NAT-PMP or its
 * successor PCP instead. Both live on UDP port 5351 (RFC 6886 §3.1,
 * RFC 6887 §19.1), so ONE socket serves both and the version byte
 * selects the dialect.
 *
 * Ordering is RFC 6887 Appendix A / §9: a client supporting both SHOULD
 * send its request in the PCP format. A NAT-PMP-only gateway answers
 * UNSUPP_VERSION with version 0, which §9 step 4 defines as "this is a
 * NAT-PMP server" — at which point we resend in NAT-PMP format.
 *
 * NO NEW LIBRARY. Everything below is raw UDP over the POSIX socket API
 * (Winsock on _WIN32), so there is nothing to add to build-deps.sh.
 *
 * The codec (build/parse) is split out from the socket loop so tests can
 * pin the wire bytes against the RFC layout without a network, and so
 * the socket loop has exactly one place where a forged or misdirected
 * datagram can be rejected.
 */

#include <stdbool.h>
#include <stdint.h>

#include "netplay/upnp.h" /* UpnpMapping + PortMapBackend (plan §9: "behind
                           * the same UpnpMapping interface") */

#ifdef __cplusplus
extern "C" {
#endif

/* --- wire constants ---------------------------------------------------- */

/** Gateway UDP port. RFC 6886 §3.1 ("port 5351 of its configured gateway
 *  address"); RFC 6887 §19.1 reassigns 5350/5351 from NAT-PMP to PCP. */
#define NATPMP_GATEWAY_PORT 5351

/** PCP protocol version. RFC 6887 §7.1 ("This document specifies protocol
 *  version 2"). */
#define NATPMP_PCP_VERSION 2
/** PCP MAP Opcode. RFC 6887 §19.2 opcode registry: 0 ANNOUNCE, 1 MAP,
 *  2 PEER. */
#define NATPMP_PCP_OPCODE_MAP 1
/** R bit — high bit of the second octet. RFC 6887 §7.1/§7.2: "R:
 *  Indicates Request (0) or Response (1)." */
#define NATPMP_PCP_R_BIT 0x80u
/** Mapping Nonce width. RFC 6887 §11.1 "Mapping Nonce (96 bits)". */
#define NATPMP_PCP_NONCE_LEN 12
/** Common header (RFC 6887 §7.1/§7.2) is 24 bytes; MAP opcode-specific
 *  data (§11.1/§11.2) is 36 bytes. Requests and responses are both 60. */
#define NATPMP_PCP_HDR_LEN 24
#define NATPMP_PCP_MAP_LEN 60

/** IANA protocol number for UDP, carried in the PCP MAP Protocol field.
 *  RFC 6887 §11.1: "This field contains 17 (UDP) if the Opcode is
 *  intended to create a UDP mapping." */
#define NATPMP_PROTO_UDP 17

/** NAT-PMP version byte. RFC 6886 §3.2/§3.3 ("Vers = 0"). */
#define NATPMP_PMP_VERSION 0
/** NAT-PMP opcodes. RFC 6886 §3.2 (OP = 0, public address) and §3.3
 *  ("Opcodes supported: 1 - Map UDP, 2 - Map TCP"). */
#define NATPMP_PMP_OP_PUBLIC_ADDR 0
#define NATPMP_PMP_OP_MAP_UDP 1
#define NATPMP_PMP_OP_MAP_TCP 2
/** Responses set the top bit of the opcode. RFC 6886 §3.2/§3.3
 *  ("OP = 128 + x"). */
#define NATPMP_PMP_RESP_FLAG 128

/** Frame lengths, RFC 6886 §3.2 (2-byte request, 12-byte response) and
 *  §3.3 (12-byte request, 16-byte response). */
#define NATPMP_PMP_ADDR_REQ_LEN 2
#define NATPMP_PMP_ADDR_RESP_LEN 12
#define NATPMP_PMP_MAP_REQ_LEN 12
#define NATPMP_PMP_MAP_RESP_LEN 16

/** PCP result codes. RFC 6887 §7.4. */
typedef enum {
    NATPMP_PCP_SUCCESS = 0,
    NATPMP_PCP_UNSUPP_VERSION = 1,
    NATPMP_PCP_NOT_AUTHORIZED = 2,
    NATPMP_PCP_MALFORMED_REQUEST = 3,
    NATPMP_PCP_UNSUPP_OPCODE = 4,
    NATPMP_PCP_UNSUPP_OPTION = 5,
    NATPMP_PCP_MALFORMED_OPTION = 6,
    NATPMP_PCP_NETWORK_FAILURE = 7,
    NATPMP_PCP_NO_RESOURCES = 8,
    NATPMP_PCP_UNSUPP_PROTOCOL = 9,
    NATPMP_PCP_USER_EX_QUOTA = 10,
    NATPMP_PCP_CANNOT_PROVIDE_EXTERNAL = 11,
    NATPMP_PCP_ADDRESS_MISMATCH = 12,
    NATPMP_PCP_EXCESSIVE_REMOTE_PEERS = 13,
} NatpmpPcpResultCode;

/** NAT-PMP result codes. RFC 6886 §3.5. */
typedef enum {
    NATPMP_PMP_SUCCESS = 0,
    NATPMP_PMP_UNSUPPORTED_VERSION = 1,
    NATPMP_PMP_NOT_AUTHORIZED = 2,
    NATPMP_PMP_NETWORK_FAILURE = 3,
    NATPMP_PMP_OUT_OF_RESOURCES = 4,
    NATPMP_PMP_UNSUPPORTED_OPCODE = 5,
} NatpmpPmpResultCode;

/* --- parse verdicts ---------------------------------------------------- */

/**
 * Why a parse says no. The distinction matters: NOT_OURS means "keep
 * listening, this datagram was not an answer to our request" (a forged
 * or crossed frame must never end the wait, let alone become a mapping),
 * while REFUSED means "the gateway answered our request and said no" —
 * a terminal, reportable outcome.
 */
typedef enum {
    NATPMP_PARSE_OK = 0,   /**< well-formed answer to OUR request, result 0 */
    NATPMP_PARSE_NOT_OURS, /**< malformed / not ours — ignore and keep waiting */
    NATPMP_PARSE_REFUSED,  /**< ours, non-zero result code                    */
    /** PCP-only: the gateway replied UNSUPP_VERSION carrying version 0.
     *  RFC 6887 §9 step 4: "If the version number in the UNSUPP_VERSION
     *  response is zero then that means this is a NAT-PMP server
     *  [RFC6886], and a client MAY choose to communicate with it using
     *  the older NAT-PMP protocol, as described in Appendix A." */
    NATPMP_PARSE_PCP_IS_NATPMP,
} NatpmpParse;

/* --- PCP MAP codec ----------------------------------------------------- */

typedef struct {
    uint32_t lifetime_s;     /**< common header Lifetime (RFC 6887 §7.2)    */
    uint32_t epoch_s;        /**< common header Epoch Time (§7.2)           */
    uint16_t internal_port;  /**< echoed from the request (§11.2)           */
    uint16_t external_port;  /**< Assigned External Port (§11.2)            */
    uint32_t external_ip_be; /**< Assigned External IP, IPv4, network order */
    uint8_t result_code;     /**< §7.4                                      */
} NatpmpPcpMap;

/**
 * Build a 60-byte PCP MAP request: common request header (RFC 6887 §7.1,
 * 24 bytes) followed by the MAP opcode-specific block (§11.1, 36 bytes).
 *
 * @param client_ip_be                 our source IPv4 in network order; §7.1
 *                                     requires the source address of the very
 *                                     datagram carrying the request, encoded
 *                                     as an IPv4-mapped IPv6 address per §5.
 * @param suggested_external_ip_be     0 for "no preference" (§11.1 says use
 *                                     the all-zeros address of the family);
 *                                     on renewal, the currently assigned
 *                                     external address (§11.2.1).
 * @param lifetime_s                   0 means delete (§11.1).
 * @return false only on a NULL argument.
 */
bool Natpmp_BuildPcpMapRequest(uint8_t out[NATPMP_PCP_MAP_LEN],
                               const uint8_t nonce[NATPMP_PCP_NONCE_LEN],
                               uint8_t protocol,
                               uint16_t internal_port,
                               uint16_t suggested_external_port,
                               uint32_t suggested_external_ip_be,
                               uint32_t client_ip_be,
                               uint32_t lifetime_s);

/**
 * Parse a PCP MAP response against the request we sent.
 *
 * Everything that does not provably belong to OUR request is
 * NATPMP_PARSE_NOT_OURS and leaves *out zeroed: short frame, wrong
 * version, R bit clear, wrong opcode, wrong Mapping Nonce, wrong
 * Protocol, wrong Internal Port, or an Assigned External IP that is not
 * an IPv4-mapped IPv6 address (RFC 6887 §5 requires all 96 leading bits
 * be checked, and this client is IPv4-only).
 *
 * The version-0 test runs FIRST and deliberately before the R-bit test:
 * a NAT-PMP gateway's "Unsupported Version" frame (RFC 6886 §3.5) is
 * Vers=0, OP=0, Result Code=1 — its OP byte has the top bit CLEAR, so an
 * R-bit-first parser would discard the very frame RFC 6887 §9 tells us to
 * act on.
 */
NatpmpParse Natpmp_ParsePcpMapResponse(const uint8_t* buf, int len,
                                       const uint8_t nonce[NATPMP_PCP_NONCE_LEN],
                                       uint8_t protocol,
                                       uint16_t internal_port,
                                       NatpmpPcpMap* out);

/* --- NAT-PMP codec ----------------------------------------------------- */

typedef struct {
    uint16_t result_code;    /**< RFC 6886 §3.5                       */
    uint32_t epoch_s;        /**< Seconds Since Start of Epoch (§3.6) */
    uint32_t external_ip_be; /**< External IPv4 Address (§3.2)        */
} NatpmpPmpAddr;

typedef struct {
    uint16_t result_code;
    uint32_t epoch_s;
    uint16_t internal_port;
    uint16_t external_port; /**< Mapped External Port (§3.3)          */
    uint32_t lifetime_s;    /**< Port Mapping Lifetime in Seconds     */
} NatpmpPmpMap;

/** Build the 2-byte Public Address Request. RFC 6886 §3.2: Vers=0, OP=0. */
bool Natpmp_BuildPmpAddrRequest(uint8_t out[NATPMP_PMP_ADDR_REQ_LEN]);

/** Parse the 12-byte Public Address Response (RFC 6886 §3.2). */
NatpmpParse Natpmp_ParsePmpAddrResponse(const uint8_t* buf, int len,
                                        NatpmpPmpAddr* out);

/**
 * Build the 12-byte "Create a Mapping" request. RFC 6886 §3.3:
 * Vers=0, OP=x, Reserved(2)=0, Internal Port(2), Suggested External
 * Port(2), Requested Port Mapping Lifetime in Seconds(4), all multi-byte
 * fields in network byte order.
 *
 * Deletion (§3.4) is this same frame with lifetime 0, and the Suggested
 * External Port "MUST be set to zero by the client on sending" — this
 * builder ENFORCES that rather than trusting the caller.
 */
bool Natpmp_BuildPmpMapRequest(uint8_t out[NATPMP_PMP_MAP_REQ_LEN],
                               uint8_t opcode,
                               uint16_t internal_port,
                               uint16_t suggested_external_port,
                               uint32_t lifetime_s);

/**
 * Parse the 16-byte mapping response (RFC 6886 §3.3). Rejects as
 * NATPMP_PARSE_NOT_OURS: short frame, version != 0, an OP that is not
 * 128 + the opcode we sent ("The 'x' in the OP field MUST match what the
 * client requested"), or an Internal Port other than the one we asked
 * for (§3.5 makes the echoed Internal Port the request identifier —
 * NAT-PMP has no transaction IDs).
 */
NatpmpParse Natpmp_ParsePmpMapResponse(const uint8_t* buf, int len,
                                       uint8_t req_opcode,
                                       uint16_t req_internal_port,
                                       NatpmpPmpMap* out);

/* --- client ------------------------------------------------------------ */

/** Wall-clock budget for a first-time probe: enough for the PCP ladder
 *  and then the NAT-PMP ladder (see natpmp.c's truncation rationale). */
#define NATPMP_PROBE_BUDGET_MS 4000
/** Wall-clock budget for a renewal, which knows its backend and so runs
 *  exactly one ladder. Kept under direct_p2p.c's
 *  UPNP_RENEW_JOIN_BUDGET_MS (2000) so a renewal worker is always
 *  joinable rather than detached. */
#define NATPMP_RENEW_BUDGET_MS 1500

/** Lifetime we request, in seconds. Matches upnp.c's UPNP_LEASE_DURATION
 *  ("3600") so the ONE S1 half-life renewal timer in direct_p2p.c is
 *  correct for every backend. */
#define NATPMP_LEASE_SECONDS 3600u

/**
 * Create (or refresh) a UDP port mapping via PCP, falling back to
 * NAT-PMP. On success fills *out with active=true, external_ip filled,
 * external/internal port, granted lifetime_s, and backend set to
 * PORTMAP_BACKEND_PCP or PORTMAP_BACKEND_NATPMP.
 *
 * @param suggested_external 0 for "no preference"; on renewal pass the
 *        currently assigned external port (RFC 6886 §3.3 / RFC 6887
 *        §11.2.1 both make this the recovery path after a gateway
 *        reboot).
 * @param backend_hint PORTMAP_BACKEND_NONE probes PCP then NAT-PMP;
 *        PORTMAP_BACKEND_PCP / _NATPMP run only that dialect (renewal).
 * @param budget_ms hard wall-clock ceiling for the whole call.
 *
 * Fails closed: no gateway, silence, a refusal, or any response that is
 * not provably ours all return false with *out zeroed.
 */
bool Natpmp_AddMapping(UpnpMapping* out,
                       uint16_t internal_port,
                       uint16_t suggested_external,
                       PortMapBackend backend_hint,
                       int budget_ms);

/**
 * Release a mapping created by Natpmp_AddMapping. Best-effort and
 * bounded, exactly like Upnp_RemoveMapping: RFC 6886 §3.1 notes deletion
 * requests "are in some sense advisory", so one send plus a short wait is
 * enough. Always clears mapping->active.
 */
void Natpmp_RemoveMapping(UpnpMapping* mapping);

#ifdef NETPLAY_TEST_HOOKS
/**
 * S7 test seam, mirroring Stun_TestHook_SetServers (stun.h:159-172):
 * point the client at a numeric-IP mock gateway (tests use a localhost
 * UDP mock on an ephemeral port) instead of the discovered default
 * route. Pass (NULL, 0) to restore real gateway discovery.
 *
 * Existence of this hook is also what keeps the harness off the
 * developer's real router: with it set, Natpmp_AddMapping never consults
 * /proc/net/route at all.
 */
void Natpmp_TestHook_SetGateway(const char* ip, uint16_t port);

/** Expose gateway discovery so a test can assert what the platform
 *  actually reports (and that non-Linux honestly reports nothing). */
bool Natpmp_TestHook_DiscoverGateway(char* out_ip, int ip_buf_size);
#endif /* NETPLAY_TEST_HOOKS */

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_NATPMP_H */
