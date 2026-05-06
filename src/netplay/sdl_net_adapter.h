#ifndef SDL_NET_ADAPTER_H
#define SDL_NET_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "gekkonet.h"

#ifdef __cplusplus
extern "C" {
#endif

struct NET_DatagramSocket;

GekkoNetAdapter* SDLNetAdapter_Create(struct NET_DatagramSocket* sock);
void SDLNetAdapter_Destroy();

// === Tier-1 netplay diag — Item 3 ===
// Per-packet-type send/receive counters (indexed by `data[0] & 7`, where
// data[0] is the GekkoNet PacketType byte at index 0 of every wire packet
// per third_party/GekkoNet/build/include/net.h:28-36). Both arrays are size
// 8 to match the 3-bit mask. Cumulative since process start; the heartbeat
// in netplay.c maintains its own previous-snapshot statics for delta math.
//
// Single producer, single consumer (the netplay thread runs send/recv and
// the heartbeat caller), so no atomics needed — the snapshot is a plain
// memcpy.
void SDLNetAdapter_SnapshotCounters(uint64_t tx[8], uint64_t rx[8]);

// === Tier-1 netplay diag — Item 4 ===
// Dump the per-packet ring buffer (last ~512 packets) to a file under the
// pref-path/states/ directory. Walks newest-to-oldest so the most recent
// activity is at the top. Filename is
// `<pref>/states/netplay_packet_ring_<utc_ms>_<role>.txt` so two peers'
// dumps don't collide. `role` is the caller's tag, e.g. "host" / "joiner"
// / "sigterm". `session_uuid` is interpolated into the file header for
// cross-log correlation. No-ops gracefully if pref-path can't be reached.
void SDLNetAdapter_DumpPacketRing(uint64_t utc_ms,
                                  const char* role,
                                  uint32_t session_uuid);

#ifdef __cplusplus
}
#endif

#endif
