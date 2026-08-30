/**
 * net_tuning.h — Socket tuning helpers for SDL3_Net datagram sockets.
 *
 * SDL3_Net's NET_DatagramSocket is opaque, but we need raw socket access
 * to set SO_RCVBUF.  This header mirrors the internal struct layout so
 * we can reach the platform socket handle without modifying third-party
 * code.
 *
 * WARNING: This layout must match SDL3_Net's internal struct.  Our
 * SDL3_Net pin is SDL3_NET_REF="92022dc" (build-deps.sh:812).  If it is
 * ever bumped, re-run the Step 1 verification recipe from
 * docs/plan-stun-direct-p2p.md:
 *
 *   rm -rf /tmp/sdl_net_verify
 *   git clone https://github.com/libsdl-org/SDL_net /tmp/sdl_net_verify
 *   git -C /tmp/sdl_net_verify checkout <new-ref>
 *   grep -n 'struct NET_DatagramSocket' /tmp/sdl_net_verify/src/SDL_net.c
 *
 * Then diff the field list below against that struct.  Field order and
 * types must match up through `handles`; trailing fields are not
 * mirrored.  Last verified against 92022dc on 2026-04-20.
 */
#ifndef NET_TUNING_H
#define NET_TUNING_H

#include <SDL3_net/SDL_net.h>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET NetRawSocket;
#else
#include <sys/socket.h>
typedef int NetRawSocket;
#endif

/* ---- Mirror of SDL3_Net internals (read-only) ---- */

typedef enum {
    NET_TUNING_SOCKETTYPE_STREAM,
    NET_TUNING_SOCKETTYPE_DATAGRAM,
    NET_TUNING_SOCKETTYPE_SERVER
} NetTuningSockType;

_Static_assert(sizeof(NetTuningSockType) == sizeof(int),
               "enum size drift breaks mirror");

typedef struct {
    NetRawSocket handle;
    int family;
    int protocol;
} NetTuningHandle;

/*
 * Partial mirror of struct NET_DatagramSocket.
 * Only the fields up to and including handles/num_handles are needed.
 */
typedef struct {
    NetTuningSockType socktype;
    NET_Address* addr;
    uint16_t port;
    int percent_loss;
    uint8_t recv_buffer[64 * 1024];
    NET_Address* latest_recv_addrs[64];
    int latest_recv_addrs_idx;
    int num_handles;
    NetTuningHandle* handles;
    /* ... remaining fields omitted ... */
} NetTuningDgramMirror;

/**
 * Set SO_RCVBUF on all handles of a NET_DatagramSocket.
 * buf_size is in bytes (e.g. 256 * 1024 for 256KB).
 * Returns the number of handles successfully tuned.
 */
static inline int NetTuning_SetRecvBuf(NET_DatagramSocket* sock, int buf_size) {
    if (!sock || buf_size <= 0)
        return 0;
    const NetTuningDgramMirror* m = (const NetTuningDgramMirror*)sock;
    int tuned = 0;
    for (int i = 0; i < m->num_handles; i++) {
        if (setsockopt((int)m->handles[i].handle, SOL_SOCKET, SO_RCVBUF, (const char*)&buf_size, sizeof(buf_size)) ==
            0) {
            tuned++;
        }
    }
    return tuned;
}

#endif /* NET_TUNING_H */
