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

#ifdef __cplusplus
}
#endif

#endif
