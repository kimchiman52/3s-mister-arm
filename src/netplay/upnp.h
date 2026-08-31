#ifndef NETPLAY_UPNP_H
#define NETPLAY_UPNP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* S7 (docs/plan-netplay-connection.md §9): which protocol actually
 * installed the mapping. There are now three mapping backends sharing
 * this one struct, and teardown/renewal MUST dispatch on it —
 * Upnp_RemoveMapping talks miniupnpc HTTP to an IGD and would be a
 * no-op-at-best, wrong-mapping-removal-at-worst against a NAT-PMP/PCP
 * gateway, and vice versa. NONE is 0 so a memset-zeroed mapping is
 * "nothing installed", which is what every existing reset site means. */
typedef enum {
    PORTMAP_BACKEND_NONE = 0,
    PORTMAP_BACKEND_UPNP = 1,   /* miniupnpc / UPnP IGD  (upnp.c)   */
    PORTMAP_BACKEND_NATPMP = 2, /* RFC 6886              (natpmp.c) */
    PORTMAP_BACKEND_PCP = 3,    /* RFC 6887              (natpmp.c) */
} PortMapBackend;

typedef struct {
    char external_ip[64];
    uint16_t external_port; // Host byte order
    uint16_t internal_port; // Host byte order
    bool active;            // True if mapping was created
    PortMapBackend backend; // S7: which protocol installed it
    /* #150: the transport protocol the mapping was CREATED with, so
     * Upnp_RemoveMapping deletes the entry it actually owns instead of
     * hardcoding "UDP" (Upnp_AddMapping has always taken the protocol as
     * a parameter; the remove side silently assumed UDP). Empty string
     * (memset-zeroed struct, or a pre-#150 mapping) means UDP — every
     * mapping this program has ever created is UDP, so the fallback is
     * exact, not a guess. NAT-PMP/PCP mappings are UDP by the request
     * opcode itself (natpmp.c) and never reach Upnp_RemoveMapping (the
     * backend gate there refuses them). */
    char protocol[8];
    /* S7: lifetime the gateway actually GRANTED, in seconds; 0 when the
     * backend does not report one (UPnP — miniupnpc's AddPortMapping has
     * no granted-lease out-parameter, so the caller falls back to its
     * static half-of-UPNP_LEASE_DURATION timer). NAT-PMP/PCP both return
     * a lifetime the gateway "MAY reduce ... from what the client
     * requested" (RFC 6886 §3.3), so renewing on a fixed half-hour timer
     * would silently lose a short-leased mapping. */
    uint32_t lifetime_s;
} UpnpMapping;

/// Attempt to create a UPnP port mapping.
/// internal_port: local port to forward to.
/// external_port: requested external port (may differ if already in use).
/// protocol: "UDP" or "TCP".
/// Returns true on success.
bool Upnp_AddMapping(UpnpMapping* out, uint16_t internal_port, uint16_t external_port, const char* protocol);

/// Remove a previously created UPnP port mapping.
void Upnp_RemoveMapping(UpnpMapping* mapping);

/// Get the external (public) IP via UPnP.
/// Returns true on success.
bool Upnp_GetExternalIP(char* out_ip, int ip_buf_size);

/// Invalidate the cached IGD URLs (forces re-discovery on next call).
void Upnp_InvalidateCache(void);

#ifdef NETPLAY_TEST_HOOKS
/// Number of times upnp.c has entered its one and only upnpDiscover()
/// call — i.e. how many times this process has attempted SSDP discovery
/// on the local network. In a harness build (NETPLAY_TEST_HOOKS AND
/// ENABLE_NETPLAY_TESTS) the refusal in upnp_ensure_cached() sits above
/// that call, so this must stay 0 for the life of the process; a
/// non-zero reading means the refusal is gone and the test binary is
/// talking to the developer's real router.
int Upnp_TestHook_DiscoverAttempts(void);

/// Zero the counter above (per-test isolation).
void Upnp_TestHook_ResetDiscoverAttempts(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
