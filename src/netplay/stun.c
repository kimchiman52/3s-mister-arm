/**
 * @file stun.c
 * @brief Minimal STUN client (RFC 5389) and endpoint encoder/decoder.
 *
 * Performs a STUN Binding Request to discover the public IP:port,
 * and provides 8-character Base64-like encoding for sharing endpoints.
 */
#ifndef _WIN32
#define _GNU_SOURCE // Must be before any includes for getaddrinfo/timeval
#endif
#include "netplay/stun.h"
#include "netplay/net_tuning.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3_net/SDL_net.h>

// Platform headers for inet_ntop
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif

// STUN message types (RFC 5389)
#define STUN_BINDING_REQUEST 0x0001
#define STUN_BINDING_RESPONSE 0x0101
#define STUN_MAGIC_COOKIE 0x2112A442

// STUN attribute types
#define STUN_ATTR_MAPPED_ADDRESS 0x0001
#define STUN_ATTR_XOR_MAPPED_ADDRESS 0x0020

void Stun_EncodeEndpoint(const char* ip, uint16_t port, uint16_t local_port, char* out_code) {
    if (!ip || !out_code)
        return;
    snprintf(out_code, 64, "%s|%u|%u", ip, port, local_port);
}

bool Stun_DecodeEndpoint(const char* code, char* out_ip, uint16_t* out_port, uint16_t* out_local_port) {
    if (!code || !out_ip || !out_port)
        return false;

    char temp[64];
    SDL_strlcpy(temp, code, sizeof(temp));

    char* sep = strchr(temp, '|');
    if (!sep)
        return false;

    *sep = '\0';
    SDL_strlcpy(out_ip, temp, 64);
    *out_port = (uint16_t)atoi(sep + 1);

    if (out_local_port) {
        char* sep2 = strchr(sep + 1, '|');
        if (sep2) {
            *out_local_port = (uint16_t)atoi(sep2 + 1);
        } else {
            *out_local_port = *out_port; // default to public port if not present
        }
    }

    return true;
}

// Build a 20-byte STUN Binding Request (RFC 5389 §6)
static void build_binding_request(uint8_t* buf, uint8_t* transaction_id) {
    // Type: Binding Request (0x0001)
    buf[0] = 0x00;
    buf[1] = 0x01;
    // Length: 0 (no attributes)
    buf[2] = 0x00;
    buf[3] = 0x00;
    // Magic Cookie
    buf[4] = 0x21;
    buf[5] = 0x12;
    buf[6] = 0xA4;
    buf[7] = 0x42;
    // Transaction ID (12 random bytes)
    for (int i = 0; i < 12; i++) {
        transaction_id[i] = (uint8_t)(SDL_rand(256));
        buf[8 + i] = transaction_id[i];
    }
}

// Parse STUN Binding Response for XOR-MAPPED-ADDRESS or MAPPED-ADDRESS
static bool parse_binding_response(const uint8_t* buf, int len, const uint8_t* transaction_id, char* out_ip,
                                   int ip_buf_size, uint16_t* out_port) {
    if (len < 20)
        return false;

    // Check message type = Binding Success Response
    uint16_t msg_type = ((uint16_t)buf[0] << 8) | buf[1];
    if (msg_type != STUN_BINDING_RESPONSE)
        return false;

    uint16_t msg_len = ((uint16_t)buf[2] << 8) | buf[3];
    if (20 + msg_len > len)
        return false;

    // Verify magic cookie
    uint32_t cookie = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
    if (cookie != STUN_MAGIC_COOKIE)
        return false;

    // Verify transaction ID
    if (memcmp(&buf[8], transaction_id, 12) != 0)
        return false;

    // Walk attributes
    int offset = 20;
    while (offset + 4 <= 20 + (int)msg_len) {
        uint16_t attr_type = ((uint16_t)buf[offset] << 8) | buf[offset + 1];
        uint16_t attr_len = ((uint16_t)buf[offset + 2] << 8) | buf[offset + 3];
        offset += 4;

        if (offset + attr_len > 20 + (int)msg_len)
            break;

        if (attr_type == STUN_ATTR_XOR_MAPPED_ADDRESS && attr_len >= 8) {
            // Family at offset+1 (skip reserved byte)
            uint8_t family = buf[offset + 1];
            uint16_t xport = ((uint16_t)buf[offset + 2] << 8) | buf[offset + 3];
            *out_port = SDL_Swap16BE(xport ^ (uint16_t)(STUN_MAGIC_COOKIE >> 16));

            if (family == 0x01) { // IPv4
                uint32_t xaddr = ((uint32_t)buf[offset + 4] << 24) | ((uint32_t)buf[offset + 5] << 16) |
                                 ((uint32_t)buf[offset + 6] << 8) | buf[offset + 7];
                uint32_t decoded_ip = SDL_Swap32BE(xaddr ^ STUN_MAGIC_COOKIE);
                uint8_t* b = (uint8_t*)&decoded_ip;
                snprintf(out_ip, ip_buf_size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                return true;
            } else if (family == 0x02 && attr_len >= 20) { // IPv6
                uint8_t decoded_ipv6[16];
                // XOR first 4 bytes with magic cookie
                decoded_ipv6[0] = buf[offset + 4] ^ (uint8_t)(STUN_MAGIC_COOKIE >> 24);
                decoded_ipv6[1] = buf[offset + 5] ^ (uint8_t)(STUN_MAGIC_COOKIE >> 16);
                decoded_ipv6[2] = buf[offset + 6] ^ (uint8_t)(STUN_MAGIC_COOKIE >> 8);
                decoded_ipv6[3] = buf[offset + 7] ^ (uint8_t)(STUN_MAGIC_COOKIE);
                // XOR remaining 12 bytes with transaction ID
                for (int i = 0; i < 12; i++) {
                    decoded_ipv6[4 + i] = buf[offset + 8 + i] ^ transaction_id[i];
                }

                // Format directly via inet_ntop
#ifdef _WIN32
                // We use Windows-compatible inet_ntop mapping (requires ws2tcpip.h which is included)
                inet_ntop(AF_INET6, decoded_ipv6, out_ip, ip_buf_size);
#else
                inet_ntop(AF_INET6, decoded_ipv6, out_ip, ip_buf_size);
#endif
                return true;
            }
        }

        if (attr_type == STUN_ATTR_MAPPED_ADDRESS && attr_len >= 8) {
            uint8_t family = buf[offset + 1];
            *out_port = SDL_Swap16BE(((uint16_t)buf[offset + 2] << 8) | buf[offset + 3]);

            if (family == 0x01) {
                uint32_t decoded_ip =
                    SDL_Swap32BE(((uint32_t)buf[offset + 4] << 24) | ((uint32_t)buf[offset + 5] << 16) |
                                 ((uint32_t)buf[offset + 6] << 8) | buf[offset + 7]);
                uint8_t* b = (uint8_t*)&decoded_ip;
                snprintf(out_ip, ip_buf_size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                return true;
            } else if (family == 0x02 && attr_len >= 20) {
#ifdef _WIN32
                inet_ntop(AF_INET6, &buf[offset + 4], out_ip, ip_buf_size);
#else
                inet_ntop(AF_INET6, &buf[offset + 4], out_ip, ip_buf_size);
#endif
                return true;
            }
        }

        // Advance to next attribute (padded to 4-byte boundary)
        offset += (attr_len + 3) & ~3;
    }

    return false;
}

// Fallback STUN server list — tried in order until one succeeds.
static const struct {
    const char* host;
    uint16_t port;
} stun_servers[] = {
    { "stun.l.google.com", 19302 },
    { "stun1.l.google.com", 19302 },
    { "stun.cloudflare.com", 3478 },
    { "stun.nextcloud.com", 443 },
};
#define STUN_SERVER_COUNT (int)(sizeof(stun_servers) / sizeof(stun_servers[0]))

bool Stun_Discover(StunResult* result, uint16_t local_port) {
    if (!result)
        return false;
    memset(result, 0, sizeof(*result));
    result->socket = NULL;

    // Create socket once — local port stays consistent across server attempts.
    // Force IPv4: MiSTer kernel disables IPv6 (sysctl net.ipv6.conf.*.disable_ipv6=1),
    // and SDL3_net's default NULL bind_addr path tries AF_INET6 first → EAFNOSUPPORT.
    // Resolve "0.0.0.0" synchronously to an IPv4 wildcard NET_Address and bind to it.
    NET_Address* bind_addr = NET_ResolveHostname("0.0.0.0");
    if (bind_addr) {
        int wait = 0;
        while (NET_GetAddressStatus(bind_addr) == NET_WAITING && wait < 100) {
            SDL_Delay(1);
            wait++;
        }
        if (NET_GetAddressStatus(bind_addr) != 1) {
            NET_UnrefAddress(bind_addr);
            bind_addr = NULL;
        }
    }
    NET_DatagramSocket* sock = NET_CreateDatagramSocket(bind_addr, local_port);
    if (bind_addr) {
        NET_UnrefAddress(bind_addr);
    }
    if (!sock) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "STUN: Failed to create UDP socket: %s", SDL_GetError());
        return false;
    }

    // Increase receive buffer to 256KB to absorb bursts when the game loop
    // is busy re-simulating during rollback (inspired by Weyvelength SDK).
    NetTuning_SetRecvBuf(sock, 256 * 1024);

    // Try each STUN server in order until one succeeds.
    for (int srv = 0; srv < STUN_SERVER_COUNT; srv++) {
        const char* host = stun_servers[srv].host;
        uint16_t srv_port = stun_servers[srv].port;

        // Resolve hostname — prefer IPv4 since NAT traversal (UPnP + hole punch)
        // is IPv4-only. Use getaddrinfo(AF_INET) to get a dotted-quad, then pass
        // that numeric string to NET_ResolveHostname for instant resolution.
        // Falls back to plain resolve on IPv6-only networks.
        NET_Address* stun_addr = NULL;
        {
            struct addrinfo hints = { 0 };
            hints.ai_family = AF_INET; // Prefer IPv4
            hints.ai_socktype = SOCK_DGRAM;
            struct addrinfo* ai = NULL;
            char ipv4_str[64] = { 0 };

            if (getaddrinfo(host, NULL, &hints, &ai) == 0 && ai) {
                struct sockaddr_in* sin = (struct sockaddr_in*)ai->ai_addr;
                inet_ntop(AF_INET, &sin->sin_addr, ipv4_str, sizeof(ipv4_str));
                freeaddrinfo(ai);
                stun_addr = NET_ResolveHostname(ipv4_str);
            } else {
                // IPv4 unavailable — fall back to default (may return IPv6)
                if (ai)
                    freeaddrinfo(ai);
                stun_addr = NET_ResolveHostname(host);
            }
        }
        if (!stun_addr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "STUN: Failed to resolve %s", host);
            continue;
        }

        int wait_attempts = 0;
        while (NET_GetAddressStatus(stun_addr) == NET_WAITING && wait_attempts < 100) {
            SDL_Delay(1);
            wait_attempts++;
        }

        if (NET_GetAddressStatus(stun_addr) != NET_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "STUN: Failed to resolve %s", host);
            NET_UnrefAddress(stun_addr);
            continue;
        }

        // Build and send STUN request (fresh transaction ID per server)
        uint8_t request[20];
        uint8_t transaction_id[12];
        build_binding_request(request, transaction_id);

        if (!NET_SendDatagram(sock, stun_addr, srv_port, request, 20)) {
            SDL_LogDebug(
                SDL_LOG_CATEGORY_APPLICATION, "STUN: Failed to send to %s:%u: %s", host, srv_port, SDL_GetError());
            NET_UnrefAddress(stun_addr);
            continue;
        }

        // Receive response — single attempt with ~2s timeout.
        // No per-server retries; we rely on having multiple servers for resilience.
        NET_Datagram* dgram = NULL;
        for (int poll = 0; poll < 20 && !dgram; poll++) {
            NET_ReceiveDatagram(sock, &dgram);
            if (!dgram)
                SDL_Delay(100);
        }

        if (!dgram) {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION, "STUN: No response from %s:%u, trying next server", host, srv_port);
            NET_UnrefAddress(stun_addr);
            continue;
        }

        // Parse response
        char ip[64] = { 0 };
        uint16_t port = 0;
        if (!parse_binding_response((const uint8_t*)dgram->buf, dgram->buflen, transaction_id, ip, sizeof(ip), &port)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "STUN: Failed to parse response from %s:%u, trying next server",
                        host,
                        srv_port);
            NET_UnrefAddress(stun_addr);
            NET_DestroyDatagram(dgram);
            continue;
        }

        // Success!
        SDL_strlcpy(result->public_ip, ip, sizeof(result->public_ip));
        result->public_port = port;
        result->socket = sock; // Keep open for hole punching!
        // S1: keep the resolved server address (ref transferred to result)
        // so Stun_SendKeepalive can rebind-probe without re-resolving.
        result->server_addr = (struct NET_Address*)stun_addr;
        result->server_port = srv_port;

        // Query the ACTUAL OS-assigned local port via getsockname.
        // The STUN public port may differ from the local port on
        // non-port-preserving NATs. The hairpin bypass needs the real
        // local port so localhost connections target the correct socket.
        {
            const NetTuningDgramMirror* m = (const NetTuningDgramMirror*)sock;
            result->local_port = port; // Fallback: assume port-preserving NAT
            for (int h = 0; h < m->num_handles; h++) {
                struct sockaddr_storage sa;
                int sa_len = sizeof(sa);
                if (getsockname((int)m->handles[h].handle, (struct sockaddr*)&sa, &sa_len) == 0) {
                    if (sa.ss_family == AF_INET) {
                        result->local_port = ntohs(((struct sockaddr_in*)&sa)->sin_port);
                        break;
                    } else if (sa.ss_family == AF_INET6) {
                        result->local_port = ntohs(((struct sockaddr_in6*)&sa)->sin6_port);
                        // Keep looking for IPv4; prefer it since STUN used IPv4
                    }
                }
            }
        }

        NET_DestroyDatagram(dgram);

        if (srv > 0) {
            SDL_Log("STUN: Discovered public endpoint via %s (local port %u)", host, result->local_port);
        } else {
            SDL_Log("STUN: Discovered public endpoint (local port %u)", result->local_port);
        }

        return true;
    }

    // All servers exhausted
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "STUN: All %d servers failed", STUN_SERVER_COUNT);
    NET_DestroyDatagramSocket(sock);
    return false;
}

bool Stun_HolePunch(StunResult* local, char* peer_ip, uint16_t* peer_port, int punch_duration_ms,
                    SDL_AtomicInt* cancel_flag) {
    if (!local || !local->socket || !peer_ip || !peer_port)
        return false;

    NET_DatagramSocket* sock = local->socket;

    // Punch packet — a small identifiable payload
    const char punch_msg[] = "3SX_PUNCH";
    const int punch_interval_ms = 200; // Send every 200ms

    NET_Address* peer = NET_ResolveHostname(peer_ip);
    if (!peer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "STUN: Failed to resolve peer IP");
        return false;
    }

    int wait_attempts = 0;
    while (NET_GetAddressStatus(peer) == NET_WAITING && wait_attempts < 300) {
        SDL_Delay(10);
        wait_attempts++;
    }

    if (NET_GetAddressStatus(peer) != NET_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "STUN: Failed to resolve peer IP");
        NET_UnrefAddress(peer);
        return false;
    }

    SDL_Log("STUN: Hole punching for %dms...", punch_duration_ms);

    uint32_t start = SDL_GetTicks();
    uint32_t last_send = 0;
    bool received_response = false;

    uint16_t local_peer_port = *peer_port; // Host order — NET_SendDatagram expects host order

    while ((int)(SDL_GetTicks() - start) < punch_duration_ms) {
        // Check for cancellation
        if (cancel_flag && SDL_GetAtomicInt(cancel_flag)) {
            SDL_Log("STUN: Hole punch cancelled by caller");
            NET_UnrefAddress(peer);
            return false;
        }
        uint32_t now = SDL_GetTicks();

        // Send punch packet periodically
        if (now - last_send >= (uint32_t)punch_interval_ms || last_send == 0) {
            if (!NET_SendDatagram(sock, peer, local_peer_port, punch_msg, strlen(punch_msg))) {
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "STUN: Hole punch send failed: %s", SDL_GetError());
            }
            last_send = now;
        }

        // Try to receive from peer
        NET_Datagram* dgram = NULL;
        NET_ReceiveDatagram(sock, &dgram);

        if (dgram) {
            // Check if it's a punch from our expected peer.
            // NOTE: NET_GetAddressString returns a pointer to an internal
            // static buffer — must copy one result before the second call.
            char recv_addr[64];
            SDL_strlcpy(recv_addr, NET_GetAddressString(dgram->addr), sizeof(recv_addr));
            if (strcmp(recv_addr, NET_GetAddressString(peer)) == 0 && dgram->buflen == strlen(punch_msg) &&
                strncmp((char*)dgram->buf, punch_msg, dgram->buflen) == 0) {
                SDL_Log("STUN: Hole punch SUCCESS — received response from peer");
                received_response = true;

                /* S2 retarget fix (docs/plan-netplay-connection.md §4): a
                 * symmetric-NAT peer punches us from a per-destination
                 * mapping whose port differs from the one we were told
                 * (the rendezvous/room-code port). We deliberately accept
                 * on source-IP + payload only, so we DO learn the true
                 * translated endpoint here — but pre-fix, the confirmation
                 * sends below still targeted the ORIGINAL port captured
                 * into local_peer_port at function entry, so the symmetric
                 * peer never saw our confirmations and ITS punch timed
                 * out. Retarget every subsequent send at the observed
                 * source endpoint before confirming. */
                if (dgram->port != local_peer_port) {
                    SDL_Log("STUN: peer punched from translated port %u (expected %u) — "
                            "retargeting confirmation sends at the observed endpoint",
                            (unsigned)dgram->port, (unsigned)local_peer_port);
                }
                local_peer_port = dgram->port;

                // Update with actual received endpoint (fixes Symmetric NAT port/IP translation)
                *peer_port = dgram->port; // Host order — NET_ReceiveDatagram returns host order

                // Update peer_ip from received address (Symmetric NAT may change it)
                const char* received_ip = NET_GetAddressString(dgram->addr);
                SDL_strlcpy(peer_ip, received_ip, 64);

                /* Keep a ref on the OBSERVED source address (the accept
                 * criteria guarantee it string-matches `peer`'s IP, but
                 * using the datagram's own address is the principled
                 * target). NET_DestroyDatagram drops the datagram's ref,
                 * so take our own before destroying. */
                NET_Address* confirmed = NET_RefAddress(dgram->addr);
                NET_DestroyDatagram(dgram);

                /* Keep punching the confirmed endpoint for ~600 ms at the
                 * fast cadence so the peer — whose own punch loop may have
                 * started late or lost packets — reliably sees at least
                 * one of ours. (Replaces the old 3 x 50 ms burst, which
                 * additionally went to the WRONG port for symmetric
                 * peers.) */
                const uint32_t confirm_start = SDL_GetTicks();
                while ((int)(SDL_GetTicks() - confirm_start) < 600) {
                    if (cancel_flag && SDL_GetAtomicInt(cancel_flag)) {
                        break;
                    }
                    NET_SendDatagram(sock, confirmed != NULL ? confirmed : peer, local_peer_port,
                                     punch_msg, strlen(punch_msg));
                    SDL_Delay(50);
                }
                if (confirmed != NULL) {
                    NET_UnrefAddress(confirmed);
                }
                break;
            }
            NET_DestroyDatagram(dgram);
        } else {
            SDL_Delay(10); // Don't spin too hot if no data
        }
    }

    if (!received_response) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "STUN: Hole punch timed out after %dms. "
                    "Peer may be behind Symmetric NAT.",
                    punch_duration_ms);
    }

    NET_UnrefAddress(peer);

    return received_response;
}

void Stun_CloseSocket(StunResult* result) {
    if (result && result->socket != NULL) {
        NET_DestroyDatagramSocket(result->socket);
        result->socket = NULL;
    }
    Stun_ReleaseServerAddr(result);
}

void Stun_ReleaseServerAddr(StunResult* result) {
    if (result && result->server_addr != NULL) {
        NET_UnrefAddress((NET_Address*)result->server_addr);
        result->server_addr = NULL;
        result->server_port = 0;
    }
}

bool Stun_SendKeepalive(StunResult* result, uint8_t out_txid[12]) {
    if (!result || !result->socket || !result->server_addr || !out_txid)
        return false;

    uint8_t request[20];
    build_binding_request(request, out_txid);
    if (!NET_SendDatagram(result->socket, (NET_Address*)result->server_addr, result->server_port, request, 20)) {
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "STUN: keepalive send failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool Stun_IsBindingResponse(const uint8_t* buf, int len) {
    if (!buf || len < 20)
        return false;
    uint16_t msg_type = ((uint16_t)buf[0] << 8) | buf[1];
    if (msg_type != STUN_BINDING_RESPONSE)
        return false;
    uint32_t cookie = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
    return cookie == STUN_MAGIC_COOKIE;
}

bool Stun_ParseBindingResponse(const uint8_t* buf, int len, const uint8_t txid[12], char* out_ip, int ip_buf_size,
                               uint16_t* out_port) {
    if (!buf || !txid || !out_ip || !out_port)
        return false;
    return parse_binding_response(buf, len, txid, out_ip, ip_buf_size, out_port);
}
