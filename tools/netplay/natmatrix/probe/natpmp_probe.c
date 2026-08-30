/* natpmp_probe -- drive the REAL production NAT-PMP/PCP client at a gateway
 * and report what it got, as one JSON line.
 *
 * This is the acceptance instrument for rig/natpmp_mock.py. It does NOT
 * reimplement the protocol: it compiles src/netplay/natpmp.c unmodified and
 * calls Natpmp_AddMapping(), so what is exercised is the shipped codec, the
 * shipped PCP-then-NAT-PMP ordering (natpmp.c:1164-1310), the shipped
 * truncated retransmit ladder (natpmp.c:828) and the shipped per-phase budgets.
 * If this binary reports a mapping, the production client accepted the mock.
 *
 * The redirection seam is Natpmp_TestHook_SetGateway (natpmp.h:345), consulted
 * at natpmp.c:757-763 BEFORE the ENABLE_NETPLAY_TESTS block at :764-785 that
 * otherwise refuses to consult the real default route. That ordering is the
 * only reason a test build can be pointed at a mock at all.
 *
 * Exit codes, so "did not run" cannot look like "passed":
 *   0  a mapping was granted
 *   10 the client ran and got no mapping (silence, refusal, or a frame it
 *      rejected) -- a real measured result
 *   20 rig error (bad arguments, SDL init failure)
 */
#if !defined(NETPLAY_TEST_HOOKS)
#error "natpmp_probe requires -DNETPLAY_TEST_HOOKS: Natpmp_TestHook_SetGateway is the only way to aim the production client at the rig's mock gateway."
#endif

#include "netplay/natpmp.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_MAPPED    0
#define EXIT_NO_MAPPING 10
#define EXIT_RIG_ERROR 20

static const char* backend_name(PortMapBackend b) {
    switch (b) {
    case PORTMAP_BACKEND_NONE:   return "none";
    case PORTMAP_BACKEND_UPNP:   return "upnp";
    case PORTMAP_BACKEND_NATPMP: return "natpmp";
    case PORTMAP_BACKEND_PCP:    return "pcp";
    default:                     return "unknown";
    }
}

static void usage(void) {
    fprintf(stderr,
        "usage: natpmp_probe --gateway IP [--gw-port N] --port N\n"
        "  [--suggested N] [--backend none|pcp|natpmp] [--budget-ms N]\n"
        "  [--remove] [--quiet]\n");
}

int main(int argc, char** argv) {
    const char* gw = NULL;
    int gw_port = NATPMP_GATEWAY_PORT;
    int internal = 0, suggested = 0, budget = 0;
    bool remove_after = false, quiet = false;
    PortMapBackend hint = PORTMAP_BACKEND_NONE;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        #define NEXT() (++i < argc ? argv[i] : (usage(), exit(EXIT_RIG_ERROR), ""))
        if      (!strcmp(a, "--gateway"))   gw = NEXT();
        else if (!strcmp(a, "--gw-port"))   gw_port = atoi(NEXT());
        else if (!strcmp(a, "--port"))      internal = atoi(NEXT());
        else if (!strcmp(a, "--suggested")) suggested = atoi(NEXT());
        else if (!strcmp(a, "--budget-ms")) budget = atoi(NEXT());
        else if (!strcmp(a, "--remove"))    remove_after = true;
        else if (!strcmp(a, "--quiet"))     quiet = true;
        else if (!strcmp(a, "--backend")) {
            const char* v = NEXT();
            if      (!strcmp(v, "none"))   hint = PORTMAP_BACKEND_NONE;
            else if (!strcmp(v, "pcp"))    hint = PORTMAP_BACKEND_PCP;
            else if (!strcmp(v, "natpmp")) hint = PORTMAP_BACKEND_NATPMP;
            else { usage(); return EXIT_RIG_ERROR; }
        }
        else { fprintf(stderr, "natpmp_probe: unknown arg %s\n", a); usage(); return EXIT_RIG_ERROR; }
        #undef NEXT
    }
    if (!gw || internal <= 0 || internal > 65535) { usage(); return EXIT_RIG_ERROR; }

    if (!SDL_Init(0)) {
        fprintf(stderr, "natpmp_probe: SDL_Init failed: %s\n", SDL_GetError());
        return EXIT_RIG_ERROR;
    }
    if (quiet) {
        SDL_SetLogPriorities(SDL_LOG_PRIORITY_CRITICAL);
    }

    Natpmp_TestHook_ResetState();
    Natpmp_TestHook_SetGateway(gw, (uint16_t)gw_port);

    UpnpMapping m;
    memset(&m, 0, sizeof(m));
    const Uint64 t0 = SDL_GetTicks();
    const bool ok = Natpmp_AddMapping(&m, (uint16_t)internal, (uint16_t)suggested,
                                      hint, budget);
    const Uint64 dt = SDL_GetTicks() - t0;

    uint32_t jitter = 0;
    const bool epoch_reset = Natpmp_TakeEpochReset(&jitter);

    printf("{\"ran\":true,\"ok\":%s,\"backend\":\"%s\",\"external_ip\":\"%s\","
           "\"external_port\":%u,\"internal_port\":%u,\"lifetime_s\":%u,"
           "\"active\":%s,\"ms\":%llu,\"epoch_reset\":%s}\n",
           ok ? "true" : "false", backend_name(m.backend), m.external_ip,
           (unsigned)m.external_port, (unsigned)m.internal_port,
           (unsigned)m.lifetime_s, m.active ? "true" : "false",
           (unsigned long long)dt, epoch_reset ? "true" : "false");
    fflush(stdout);

    if (ok && remove_after) {
        Natpmp_RemoveMapping(&m);
    }
    return ok ? EXIT_MAPPED : EXIT_NO_MAPPING;
}
