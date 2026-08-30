/* session_phase.c — task #119: drive the REAL post-handoff session stack
 * inside the netns rig, so the FAILED_HANDSHAKE band (and its late-connect
 * rescue) is measurable instead of being declared out of scope.
 *
 * What is real here, in the sense that it is the shipped code compiled
 * unmodified from src/ (or the shipped vendored library):
 *   - GekkoNet itself (libGekkoNet.a @ 7be848c + the build-deps.sh
 *     security patches) — the sync exchange whose one-time by-string
 *     address registration is the reason a one-sided handoff hangs;
 *   - SDLNetAdapter (src/netplay/sdl_net_adapter.c) — including the #119
 *     punch intercept, canonical-peer presentation and retarget;
 *   - late_punch.c — the rescue layer under test;
 *   - the orchestrator's FAILED_HANDSHAKE parking — a deadline here goes
 *     through DirectP2P_NotifySessionFailed + the captured teardown
 *     callback (direct_p2p_on_teardown), i.e. the production latch, in
 *     the caller (p2p_probe.c).
 *
 * What is imitated: the thin glue netplay.c wraps around those parts —
 * the per-frame call order (tick late_punch, apply relearn, update
 * session, drain events), the CONNECT_TIMEOUT_CONNECTING_MS deadline
 * (the SAME constant, from connect_fail.h), and a dummy 8-byte game
 * state. The MIST handshake gate is NOT modelled: it sits between
 * handoff and gekko in the game and is a different (versioning) gate;
 * its punch-intercept integration is the same late_punch module driven
 * from mist_netio_recv, but the rig exercises the GekkoNet phase only.
 * Stated here so nobody reads a green run as MIST coverage. */

#include "session_phase.h"

#include "netplay/connect_fail.h"
#include "netplay/late_punch.h"
#include "netplay/direct_p2p.h"
#include "netplay/sdl_net_adapter.h"

#include "gekkonet.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

int ProbeSession_Run(bool is_host, bool late_punch_on, unsigned grace_ms,
                     struct NET_DatagramSocket* sock,
                     const char* peer_ip, unsigned short peer_port,
                     const unsigned char* token, bool token_valid,
                     ProbeSessionReport* out) {
    memset(out, 0, sizeof(*out));

    if (sock == NULL || peer_ip == NULL || peer_ip[0] == '\0' || peer_port == 0) {
        fprintf(stderr, "[probe-session] missing handoff capture (sock=%p ip=%s port=%u)\n",
                (void*)sock, peer_ip ? peer_ip : "(null)", (unsigned)peer_port);
        return PROBE_SESSION_RIG_ERROR;
    }

    /* Mirror configure_gekko's shape (netplay.c): host is player 0 with
     * the remote at slot 1, joiner the reverse; 2-byte inputs. The state
     * is a dummy 8 bytes — session SYNC (what this phase measures) is a
     * magic exchange and never compares state across peers. Both sides
     * of a rig run use this identical config. */
    GekkoConfig conf;
    memset(&conf, 0, sizeof(conf));
    conf.num_players = 2;
    conf.max_spectators = 0;
    conf.input_prediction_window = 8;
    conf.input_size = 2;
    conf.state_size = 8;
    conf.limited_saving = false;
    conf.desync_detection = false;

    GekkoSession* session = NULL;
    if (!gekko_create(&session, GekkoGameSession)) {
        fprintf(stderr, "[probe-session] gekko_create failed\n");
        return PROBE_SESSION_RIG_ERROR;
    }
    gekko_start(session, &conf);
    gekko_net_adapter_set(session, SDLNetAdapter_Create(sock));

    char remote_str[100];
    snprintf(remote_str, sizeof(remote_str), "%s:%u", peer_ip, (unsigned)peer_port);
    GekkoNetAddress remote_addr = { .data = remote_str,
                                    .size = (unsigned int)strlen(remote_str) };
    /* Same registration order as netplay.c's PLAYER_COUNT loop. */
    const int local_idx = is_host ? 0 : 1;
    int local_handle = -1;
    for (int i = 0; i < 2; i++) {
        if (i == local_idx) {
            local_handle = gekko_add_actor(session, GekkoLocalPlayer, NULL);
            gekko_set_local_delay(session, local_handle, 1);
        } else {
            gekko_add_actor(session, GekkoRemotePlayer, &remote_addr);
        }
    }
    /* #119: the canonical registration the adapter translates around. */
    SDLNetAdapter_SetCanonicalPeer(peer_ip, peer_port);

    if (late_punch_on && token_valid) {
        LatePunch_Arm(sock, token, peer_ip, peer_port);
    } else {
        fprintf(stderr, "[probe-session] late-punch layer OFF (%s)\n",
                late_punch_on ? "no token captured" : "disabled by flag");
    }

    const Uint64 t0 = SDL_GetTicks();
    Uint64 t_started = 0;
    Uint64 last_input_ms = 0;
    unsigned char dummy_state[8] = { 0 };

    int rc = PROBE_SESSION_DEADLINE;
    for (;;) {
        const Uint64 now = SDL_GetTicks();

        /* Keep the orchestrator ticking exactly as the game loop does —
         * its HANDOFF case still runs its own reporting there. */
        DirectP2P_Tick();

        /* The netplay.c glue this phase imitates: late_punch_service. */
        if (LatePunch_IsArmed()) {
            LatePunch_Tick((uint32_t)now);
            char rip[64];
            unsigned short rport = 0;
            if (LatePunch_TakeRelearn(rip, sizeof(rip), &rport) && rport != 0) {
                out->relearns++;
                fprintf(stderr, "[probe-session] applying relearn -> %s:%u\n",
                        rip, (unsigned)rport);
                SDLNetAdapter_RetargetPeer(rip, rport);
            }
        }

        /* ~60 Hz local input feed, like the game loop. */
        if (now - last_input_ms >= 16) {
            last_input_ms = now;
            unsigned short input = 0;
            gekko_add_local_input(session, local_handle, &input);
        }

        int game_ev_n = 0;
        GekkoGameEvent** game_evs = gekko_update_session(session, &game_ev_n);
        for (int i = 0; i < game_ev_n; i++) {
            GekkoGameEvent* ev = game_evs[i];
            if (ev->type == GekkoSaveEvent) {
                *ev->data.save.state_len = sizeof(dummy_state);
                *ev->data.save.checksum = 0;
                memcpy(ev->data.save.state, dummy_state, sizeof(dummy_state));
            }
            /* Advance/Load: dummy state, nothing to do. */
        }

        int sess_ev_n = 0;
        GekkoSessionEvent** sess_evs = gekko_session_events(session, &sess_ev_n);
        for (int i = 0; i < sess_ev_n; i++) {
            if (sess_evs[i]->type == GekkoSessionStarted && t_started == 0) {
                t_started = now;
                out->started = true;
                out->ms_to_session = (unsigned)(now - t0);
                fprintf(stderr, "[probe-session] SessionStarted at +%u ms\n",
                        out->ms_to_session);
                /* Production parity: the rescue window closes here
                 * (netplay.c GekkoSessionStarted). */
                LatePunch_Disarm();
            }
        }

        if (t_started != 0 && now - t_started >= grace_ms) {
            rc = PROBE_SESSION_STARTED;
            break;
        }
        /* THE deadline under test: the same constant netplay.c's
         * CONNECTING case arms (connect_fail.h). The caller converts
         * this into the production FAILED_HANDSHAKE park. */
        if (t_started == 0 && now - t0 >= CONNECT_TIMEOUT_CONNECTING_MS) {
            fprintf(stderr, "[probe-session] no GekkoSessionStarted within %u ms\n",
                    (unsigned)CONNECT_TIMEOUT_CONNECTING_MS);
            break;
        }
        SDL_Delay(4);
    }

    LatePunch_Disarm();
    gekko_destroy(&session);
    SDLNetAdapter_Destroy();
    return rc;
}
