/**
 * @file upnp.c
 * @brief UPnP port mapping wrapper using miniupnpc.
 *
 * Provides a simple interface to create/remove UDP port mappings
 * on the local router via UPnP IGD protocol.
 * Compiled only when HAVE_UPNP is defined (miniupnpc available).
 */
#include "upnp.h"

#ifdef HAVE_UPNP

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

/* miniupnpc API v18+ (v2.2.8) added wan_addr params to UPNP_GetValidIGD */
#ifndef MINIUPNPC_API_VERSION
#define MINIUPNPC_API_VERSION 0
#endif

#define UPNP_DISCOVER_TIMEOUT_MS 2000
#define UPNP_LEASE_DURATION "3600" // 1 hour lease

/* ---- IGD cache ----
 * Caches the discovered IGD URLs and service data after the first successful
 * Upnp_AddMapping() call. Subsequent RemoveMapping / GetExternalIP calls
 * reuse the cache, avoiding the 2-second upnpDiscover() round-trip each time.
 */
static struct UPNPUrls s_cached_urls;
static struct IGDdatas s_cached_data;
static char s_cached_lan_addr[64];
static bool s_cache_valid = false;

/* Discover the IGD and populate the cache. Returns true on success. */
static bool upnp_ensure_cached(void) {
    if (s_cache_valid)
        return true;

    int error = 0;
    struct UPNPDev* devlist = upnpDiscover(UPNP_DISCOVER_TIMEOUT_MS, NULL, NULL, 0, 0, 2, &error);
    if (!devlist) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "UPnP: No IGD devices found (error %d)", error);
        return false;
    }

    memset(&s_cached_urls, 0, sizeof(s_cached_urls));
    memset(&s_cached_data, 0, sizeof(s_cached_data));
    memset(s_cached_lan_addr, 0, sizeof(s_cached_lan_addr));

#if MINIUPNPC_API_VERSION >= 18
    char wan_addr[64] = { 0 };
    int status = UPNP_GetValidIGD(devlist,
                                  &s_cached_urls,
                                  &s_cached_data,
                                  s_cached_lan_addr,
                                  sizeof(s_cached_lan_addr),
                                  wan_addr,
                                  sizeof(wan_addr));
#else
    int status =
        UPNP_GetValidIGD(devlist, &s_cached_urls, &s_cached_data, s_cached_lan_addr, sizeof(s_cached_lan_addr));
#endif

    freeUPNPDevlist(devlist);

    if (status != 1) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "UPnP: No valid IGD found (status %d)", status);
        FreeUPNPUrls(&s_cached_urls);
        return false;
    }

    s_cache_valid = true;
    SDL_Log("UPnP: Found IGD (cached)");
    return true;
}

bool Upnp_AddMapping(UpnpMapping* out, uint16_t internal_port, uint16_t external_port, const char* protocol) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    if (!upnp_ensure_cached())
        return false;

    // Get external IP
    char ext_ip[64] = { 0 };
    int r = UPNP_GetExternalIPAddress(s_cached_urls.controlURL, s_cached_data.first.servicetype, ext_ip);
    if (r != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "UPnP: Failed to get external IP (error %d)", r);
        return false;
    }

    // Try to add port mapping
    char int_port_str[8], ext_port_str[8];
    snprintf(int_port_str, sizeof(int_port_str), "%u", internal_port);
    snprintf(ext_port_str, sizeof(ext_port_str), "%u", external_port);

    r = UPNP_AddPortMapping(s_cached_urls.controlURL,
                            s_cached_data.first.servicetype,
                            ext_port_str,
                            int_port_str,
                            s_cached_lan_addr,
                            "3SX Netplay",
                            protocol,
                            NULL,
                            UPNP_LEASE_DURATION);

    if (r != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "UPnP: AddPortMapping failed: %s (error %d)", strupnperror(r), r);
        return false;
    }

    SDL_Log("UPnP: Port mapping created (%s)", protocol);

    SDL_strlcpy(out->external_ip, ext_ip, sizeof(out->external_ip));
    out->external_port = external_port;
    out->internal_port = internal_port;
    out->active = true;
    /* S7: stamp the backend so teardown/renewal dispatch correctly now
     * that natpmp.c fills the same struct. lifetime_s stays 0 —
     * UPNP_AddPortMapping reports no granted lease. */
    out->backend = PORTMAP_BACKEND_UPNP;
    out->lifetime_s = 0;
    return true;
}

void Upnp_RemoveMapping(UpnpMapping* mapping) {
    if (!mapping || !mapping->active)
        return;

    /* S7 fail-closed: three backends share UpnpMapping now. Deleting a
     * NAT-PMP/PCP mapping through the IGD would, on a router that speaks
     * BOTH, delete whatever unrelated IGD entry happens to sit on that
     * external port. Refuse and let the caller's dispatch bug be loud.
     * (BACKEND_NONE is accepted: mappings minted before S7 — and any
     * hand-built one in a test — carry 0 and are UPnP by construction.) */
    if (mapping->backend != PORTMAP_BACKEND_UPNP && mapping->backend != PORTMAP_BACKEND_NONE) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "UPnP: refusing to remove a mapping owned by backend %d — "
                    "wrong teardown path (see PortMapBackend dispatch)",
                    (int)mapping->backend);
        return;
    }

    if (!upnp_ensure_cached()) {
        mapping->active = false;
        return;
    }

    char ext_port_str[8];
    snprintf(ext_port_str, sizeof(ext_port_str), "%u", mapping->external_port);

    int r =
        UPNP_DeletePortMapping(s_cached_urls.controlURL, s_cached_data.first.servicetype, ext_port_str, "UDP", NULL);
    if (r == 0) {
        SDL_Log("UPnP: Port mapping removed for port %u", mapping->external_port);
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "UPnP: Failed to remove port mapping: %s", strupnperror(r));
    }

    mapping->active = false;
    Upnp_InvalidateCache();
}

bool Upnp_GetExternalIP(char* out_ip, int ip_buf_size) {
    if (!upnp_ensure_cached())
        return false;

    char ext_ip[64] = { 0 };
    if (UPNP_GetExternalIPAddress(s_cached_urls.controlURL, s_cached_data.first.servicetype, ext_ip) == 0) {
        SDL_strlcpy(out_ip, ext_ip, ip_buf_size);
        return true;
    }
    return false;
}

void Upnp_InvalidateCache(void) {
    if (s_cache_valid) {
        FreeUPNPUrls(&s_cached_urls);
        memset(&s_cached_data, 0, sizeof(s_cached_data));
        memset(s_cached_lan_addr, 0, sizeof(s_cached_lan_addr));
        s_cache_valid = false;
        SDL_Log("UPnP: IGD cache invalidated");
    }
}

#else // !HAVE_UPNP — stubs

bool Upnp_AddMapping(UpnpMapping* out, uint16_t internal_port, uint16_t external_port, const char* protocol) {
    (void)out;
    (void)internal_port;
    (void)external_port;
    (void)protocol;
    return false;
}

void Upnp_RemoveMapping(UpnpMapping* mapping) {
    (void)mapping;
}

bool Upnp_GetExternalIP(char* out_ip, int ip_buf_size) {
    (void)out_ip;
    (void)ip_buf_size;
    return false;
}

void Upnp_InvalidateCache(void) {}

#endif // HAVE_UPNP
