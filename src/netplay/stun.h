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
} StunResult;

/// Perform a STUN Binding Request (RFC 5389).
/// Tries multiple STUN servers in order (Google, Cloudflare, Nextcloud)
/// for resilience against rate limiting or service outages.
/// Returns true on success and fills `result`.
/// The socket in result->socket is left open for hole punching.
bool Stun_Discover(StunResult* result, uint16_t local_port);

/// Close the STUN socket when done
void Stun_CloseSocket(StunResult* result);

/// Encode an IP string + port into an endpoint string.
/// out_code must be at least 64 bytes.
void Stun_EncodeEndpoint(const char* ip, uint16_t port, uint16_t local_port, char* out_code);

/// Decode an endpoint string back into IP string + port.
/// out_ip must be at least 64 bytes.
/// Returns true on success.
bool Stun_DecodeEndpoint(const char* code, char* out_ip, uint16_t* out_port, uint16_t* out_local_port);

/// Hole punches NAT to connect to peer. Blocks for up to `punch_duration_ms`.
// Updates `peer_ip` and `peer_port` with the true translated endpoint if successful.
bool Stun_HolePunch(StunResult* local, char* peer_ip, uint16_t* peer_port, int punch_duration_ms,
                    SDL_AtomicInt* cancel_flag);

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

#ifdef __cplusplus
}
#endif

#endif
