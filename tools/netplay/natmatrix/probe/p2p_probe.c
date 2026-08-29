/* p2p_probe -- drive the REAL netplay connection cascade inside a network
 * namespace, and report the outcome as one JSON line.
 *
 * This is the instrument for the S8 NAT matrix. It links the production
 * orchestrator (src/netplay/direct_p2p.c) and the production STUN, rendezvous
 * and room-code modules unmodified. It does NOT reimplement the cascade: the
 * whole point is that the state machine, timers and wire formats under test are
 * the shipped ones.
 *
 * Cascade exercised: STUN discovery -> p2p_race (punch leg 0 + rendezvous signal
 * leg + DELIVER-taught punch leg 1) -> handoff. There is no relay; the S5 relay
 * was removed (rendezvous.c:33-35) and this harness deliberately does not model
 * one.
 *
 * ---------------------------------------------------------------------------
 * EXIT CODES -- "did not run" must never look like "passed".
 *
 * On this project a clean exit has repeatedly meant "never ran": the existing
 * netplay harnesses return 2 for "not compiled in", which is NOT a pass. Two
 * defences here:
 *
 *   1. The #error below makes a probe binary that lacks ENABLE_NETPLAY or
 *      NETPLAY_TEST_HOOKS IMPOSSIBLE TO BUILD. There is no runtime "not
 *      compiled in" code path to misread, because there is no such binary.
 *   2. Every run emits a JSON line containing "ran":true. The matrix driver
 *      refuses to score any cell whose probe did not emit "ran":true, so a
 *      crash, a signal or an exec failure can never be counted as a result.
 *
 *   0  CONNECTED      -- reached DIRECT_P2P_HANDOFF. The cascade succeeded.
 *   10 NOT_CONNECTED  -- ran to a terminal FAILED_* state. A real measured
 *                        failure. This is a legitimate scientific result, not
 *                        an error: for symmetric x symmetric it is the EXPECTED
 *                        outcome now that the relay is gone.
 *   20 RIG_ERROR      -- the harness itself is broken (bind failed, no STUN
 *                        answer at all, bad arguments). Never counted as data.
 *   30 TIMEOUT        -- no terminal state within the wall-clock budget.
 * ---------------------------------------------------------------------------
 */
#if !defined(ENABLE_NETPLAY)
#error "p2p_probe requires -DENABLE_NETPLAY. A probe without the cascade compiled in would exit cleanly while testing nothing."
#endif
#if !defined(NETPLAY_TEST_HOOKS)
#error "p2p_probe requires -DNETPLAY_TEST_HOOKS: Stun_TestHook_SetServers is the ONLY way to redirect STUN away from the hardcoded public servers (src/netplay/stun.c:280-285), which are unreachable from the netns."
#endif

#include "netplay/direct_p2p.h"
#include "netplay/stun.h"
#include "port/config/config.h"
#include "port/paths.h"

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_CONNECTED     0
#define EXIT_NOT_CONNECTED 10
#define EXIT_RIG_ERROR     20
#define EXIT_TIMEOUT       30

#define MAX_STUN 8
#define MAX_TRANSITIONS 32

static const char* state_name(DirectP2PState s) {
    switch (s) {
    case DIRECT_P2P_IDLE:                     return "IDLE";
    case DIRECT_P2P_UPNP_PROBE:               return "UPNP_PROBE";
    case DIRECT_P2P_STUN_DISCOVER:            return "STUN_DISCOVER";
    case DIRECT_P2P_HOST_WAITING:             return "HOST_WAITING";
    case DIRECT_P2P_JOIN_PUNCHING:            return "JOIN_PUNCHING";
    case DIRECT_P2P_HANDOFF:                  return "HANDOFF";
    case DIRECT_P2P_FAILED_SYMMETRIC:         return "FAILED_SYMMETRIC";
    case DIRECT_P2P_FAILED_STUN:              return "FAILED_STUN";
    case DIRECT_P2P_FAILED_PUNCH:             return "FAILED_PUNCH";
    case DIRECT_P2P_FALLBACK_SIGNALING:       return "FALLBACK_SIGNALING";
    case DIRECT_P2P_FALLBACK_BILATERAL_PUNCH: return "FALLBACK_BILATERAL_PUNCH";
    case DIRECT_P2P_FAILED_BILATERAL:         return "FAILED_BILATERAL";
    case DIRECT_P2P_FAILED_HANDSHAKE:         return "FAILED_HANDSHAKE";
    default:                                  return "UNKNOWN";
    }
}

static bool is_terminal(DirectP2PState s) {
    switch (s) {
    case DIRECT_P2P_HANDOFF:
    case DIRECT_P2P_FAILED_SYMMETRIC:
    case DIRECT_P2P_FAILED_STUN:
    case DIRECT_P2P_FAILED_PUNCH:
    case DIRECT_P2P_FAILED_BILATERAL:
    case DIRECT_P2P_FAILED_HANDSHAKE:
        return true;
    default:
        return false;
    }
}

/* The probe writes the config file itself rather than using Config_Set*, because
 * the integer connection knobs (punch/signal/race budgets, register interval)
 * have no Config_SetInt setter -- config.c parses integer-looking values out of
 * the file (config.c:349-375). This is also how the shipped game gets them. */
static bool write_config(const char* signal_url, int punch_ms, int signal_ms,
                         int race_ms, int register_ms, int stun_ms,
                         bool disable_bilateral) {
    const char* pref = Paths_GetPrefPath();
    if (!pref) { fprintf(stderr, "p2p_probe: Paths_GetPrefPath() returned NULL\n"); return false; }
    char path[1024];
    snprintf(path, sizeof(path), "%sconfig", pref);
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "p2p_probe: cannot write %s\n", path); return false; }
    fprintf(f, "netplay-direct-p2p-signal-url = %s\n", signal_url);
    fprintf(f, "netplay-direct-p2p-bilateral-punch-ms = %d\n", punch_ms);
    fprintf(f, "netplay-direct-p2p-signal-budget-ms = %d\n", signal_ms);
    fprintf(f, "netplay-direct-p2p-race-budget-ms = %d\n", race_ms);
    fprintf(f, "netplay-direct-p2p-register-interval-ms = %d\n", register_ms);
    fprintf(f, "netplay-direct-p2p-stun-timeout-ms = %d\n", stun_ms);
    fprintf(f, "netplay-direct-p2p-disable-bilateral = %s\n",
            disable_bilateral ? "true" : "false");
    /* UPnP and NAT-PMP have no gateway in the netns; leaving them enabled just
     * burns the discovery timeout in every single cell. */
    fprintf(f, "netplay-direct-p2p-disable-upnp = true\n");
    fprintf(f, "netplay-direct-p2p-disable-natpmp = true\n");
    fclose(f);
    return true;
}

static void usage(void) {
    fprintf(stderr,
        "usage: p2p_probe --role host|join --stun IP:PORT[,IP:PORT...] \\\n"
        "                 --signal udp://IP:PORT --code-file PATH\n"
        "  [--port N] [--timeout-ms N] [--punch-ms N] [--signal-ms N]\n"
        "  [--race-ms N] [--register-ms N] [--stun-ms N] [--disable-bilateral]\n");
}

int main(int argc, char** argv) {
    const char* role = NULL;
    const char* stun_arg = NULL;
    const char* signal_url = "udp://203.0.113.100:3478";
    const char* code_file = NULL;
    int port = 7000, timeout_ms = 60000;
    int punch_ms = 5000, signal_ms = 8000, race_ms = 8000;
    int register_ms = 5000, stun_ms = 4000;
    bool disable_bilateral = false;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        #define NEXT() (++i < argc ? argv[i] : (usage(), exit(EXIT_RIG_ERROR), ""))
        if      (!strcmp(a, "--role"))       role = NEXT();
        else if (!strcmp(a, "--stun"))       stun_arg = NEXT();
        else if (!strcmp(a, "--signal"))     signal_url = NEXT();
        else if (!strcmp(a, "--code-file"))  code_file = NEXT();
        else if (!strcmp(a, "--port"))       port = atoi(NEXT());
        else if (!strcmp(a, "--timeout-ms")) timeout_ms = atoi(NEXT());
        else if (!strcmp(a, "--punch-ms"))   punch_ms = atoi(NEXT());
        else if (!strcmp(a, "--signal-ms"))  signal_ms = atoi(NEXT());
        else if (!strcmp(a, "--race-ms"))    race_ms = atoi(NEXT());
        else if (!strcmp(a, "--register-ms"))register_ms = atoi(NEXT());
        else if (!strcmp(a, "--stun-ms"))    stun_ms = atoi(NEXT());
        else if (!strcmp(a, "--disable-bilateral")) disable_bilateral = true;
        else { fprintf(stderr, "p2p_probe: unknown arg %s\n", a); usage(); return EXIT_RIG_ERROR; }
        #undef NEXT
    }
    if (!role || !stun_arg || !code_file) { usage(); return EXIT_RIG_ERROR; }
    bool is_host = !strcmp(role, "host");
    if (!is_host && strcmp(role, "join")) { usage(); return EXIT_RIG_ERROR; }

    /* Parse --stun into the test-hook server pool. */
    static StunServerDesc servers[MAX_STUN];
    static char stun_hosts[MAX_STUN][64];
    int nstun = 0;
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", stun_arg);
        for (char* tok = strtok(buf, ","); tok && nstun < MAX_STUN; tok = strtok(NULL, ",")) {
            char* colon = strrchr(tok, ':');
            if (!colon) { fprintf(stderr, "p2p_probe: bad --stun entry '%s'\n", tok); return EXIT_RIG_ERROR; }
            *colon = '\0';
            snprintf(stun_hosts[nstun], sizeof(stun_hosts[nstun]), "%s", tok);
            servers[nstun].host = stun_hosts[nstun];
            servers[nstun].port = (uint16_t)atoi(colon + 1);
            nstun++;
        }
    }
    if (nstun == 0) { fprintf(stderr, "p2p_probe: --stun parsed to zero servers\n"); return EXIT_RIG_ERROR; }

    if (!SDL_Init(0)) { fprintf(stderr, "p2p_probe: SDL_Init failed: %s\n", SDL_GetError()); return EXIT_RIG_ERROR; }
    if (!NET_Init())  { fprintf(stderr, "p2p_probe: NET_Init failed: %s\n", SDL_GetError()); return EXIT_RIG_ERROR; }

    if (!write_config(signal_url, punch_ms, signal_ms, race_ms, register_ms,
                      stun_ms, disable_bilateral))
        return EXIT_RIG_ERROR;
    Config_Init();

    Stun_TestHook_SetServers(servers, nstun);
    DirectP2P_Init();

    Uint64 t0 = SDL_GetTicks();
    Uint64 t_handoff = 0;
    char code[64] = {0};
    bool code_published = false;

    if (is_host) {
        DirectP2P_BeginHost(port);
    } else {
        /* Wait for the host to publish its room code into the shared file. */
        Uint64 wait_deadline = t0 + (Uint64)timeout_ms;
        FILE* f = NULL;
        while (SDL_GetTicks() < wait_deadline) {
            f = fopen(code_file, "r");
            if (f) {
                if (fgets(code, sizeof(code), f)) {
                    size_t n = strlen(code);
                    while (n && (code[n-1] == '\n' || code[n-1] == '\r')) code[--n] = '\0';
                }
                fclose(f);
                if (code[0]) break;
            }
            SDL_Delay(50);
        }
        if (!code[0]) {
            printf("{\"ran\":true,\"role\":\"join\",\"outcome\":\"RIG_ERROR\","
                   "\"reason\":\"host never published a room code\"}\n");
            return EXIT_RIG_ERROR;
        }
        DirectP2P_BeginJoin(code);
    }

    /* Pump the orchestrator exactly as the game loop does, recording every state
     * transition with the millisecond it happened. */
    struct { const char* name; Uint64 ms; } trans[MAX_TRANSITIONS];
    int ntrans = 0;
    DirectP2PState last = (DirectP2PState)-1;
    DirectP2PState st = DIRECT_P2P_IDLE;
    bool timed_out = false;

    for (;;) {
        DirectP2P_Tick();
        st = DirectP2P_GetState();

        if (st != last) {
            if (ntrans < MAX_TRANSITIONS) {
                trans[ntrans].name = state_name(st);
                trans[ntrans].ms = SDL_GetTicks() - t0;
                ntrans++;
            }
            last = st;
        }

        if (is_host && !code_published && st == DIRECT_P2P_HOST_WAITING) {
            const char* c = DirectP2P_GetHostCode();
            if (c && c[0]) {
                snprintf(code, sizeof(code), "%s", c);
                /* Write to a temp file then rename, so the joiner can never read
                 * a half-written code. */
                char tmp[1024];
                snprintf(tmp, sizeof(tmp), "%s.tmp", code_file);
                FILE* f = fopen(tmp, "w");
                if (f) { fprintf(f, "%s\n", code); fclose(f); rename(tmp, code_file); }
                code_published = true;
            }
        }

        if (st == DIRECT_P2P_HANDOFF && t_handoff == 0)
            t_handoff = SDL_GetTicks() - t0;

        if (is_terminal(st)) break;

        if ((Uint64)(SDL_GetTicks() - t0) > (Uint64)timeout_ms) { timed_out = true; break; }
        SDL_Delay(5);
    }

    Uint64 total = SDL_GetTicks() - t0;
    const char* outcome = timed_out ? "TIMEOUT"
                        : (st == DIRECT_P2P_HANDOFF ? "CONNECTED" : "NOT_CONNECTED");

    printf("{\"ran\":true,\"role\":\"%s\",\"outcome\":\"%s\",\"final_state\":\"%s\","
           "\"ms_total\":%llu,\"ms_to_handoff\":%llu,\"code\":\"%s\",\"status\":\"%s\","
           "\"transitions\":[",
           is_host ? "host" : "join", outcome, state_name(st),
           (unsigned long long)total, (unsigned long long)t_handoff,
           code, DirectP2P_GetStatusText() ? DirectP2P_GetStatusText() : "");
    for (int i = 0; i < ntrans; i++)
        printf("%s{\"s\":\"%s\",\"ms\":%llu}", i ? "," : "",
               trans[i].name, (unsigned long long)trans[i].ms);
    printf("]}\n");
    fflush(stdout);

    DirectP2P_Cancel();

    if (timed_out) return EXIT_TIMEOUT;
    return (st == DIRECT_P2P_HANDOFF) ? EXIT_CONNECTED : EXIT_NOT_CONNECTED;
}
