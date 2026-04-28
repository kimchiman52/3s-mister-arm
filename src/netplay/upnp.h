#ifndef NETPLAY_UPNP_H
#define NETPLAY_UPNP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char external_ip[64];
    uint16_t external_port; // Host byte order
    uint16_t internal_port; // Host byte order
    bool active;            // True if mapping was created
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

#ifdef __cplusplus
}
#endif

#endif
