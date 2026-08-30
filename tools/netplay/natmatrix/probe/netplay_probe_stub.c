/* Game-side symbols that src/netplay/direct_p2p.c references but which the probe
 * does not need: everything past the handoff boundary.
 *
 * Pre-#119 the cascade under test ENDED at DIRECT_P2P_HANDOFF and every stub
 * discarded its argument. Task #119 extends the probe past that boundary on
 * request (p2p_probe --session): the split-brain failure it closes lives
 * BETWEEN the handoff and GekkoSessionStarted, i.e. exactly in the phase the
 * old probe declared out of scope. So the Netplay_Set* stubs now CAPTURE what
 * do_handoff hands them — socket, peer endpoint, punch token, teardown
 * callback — and session_phase.c drives the real GekkoNet + SDLNetAdapter +
 * late_punch stack on the captured state. Without --session nothing reads the
 * captures and the probe's behaviour is unchanged.
 *
 * Deliberately NOT stubbed: anything in the traversal path. stun.c, rendezvous.c,
 * room_code.c, natpmp.c, upnp.c, connect_fail.c and direct_p2p.c itself are all
 * compiled from src/ unmodified.
 */
#include "netplay_probe_stub.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void (*s_teardown_cb)(void) = NULL;
static bool s_remote_ip_set = false;

/* --- task #119 captures (see header) --- */
static struct NET_DatagramSocket* s_captured_sock = NULL;
static char s_captured_ip[64] = { 0 };
static unsigned short s_captured_port = 0;
static unsigned char s_captured_token[8] = { 0 };
static bool s_captured_token_valid = false;

void Netplay_SetParams(int player, const char* ip) {
    (void)player;
    s_remote_ip_set = (ip != NULL && ip[0] != '\0');
    if (s_remote_ip_set) {
        snprintf(s_captured_ip, sizeof(s_captured_ip), "%s", ip);
    } else {
        s_captured_ip[0] = '\0';
    }
}

void Netplay_SetRemotePort(unsigned short port) { s_captured_port = port; }

bool Netplay_IsRemoteIpSet(void) { return s_remote_ip_set; }

/* Reaching here IS the success condition of the cascade. Log it so a run's
 * stdout independently corroborates the DIRECT_P2P_HANDOFF state the probe
 * reports -- two witnesses to the same event. */
void Netplay_BeginDirectP2P(void) {
    fprintf(stderr, "[probe] Netplay_BeginDirectP2P: handoff reached\n");
}

void Netplay_SetStunSocket(struct NET_DatagramSocket* socket) {
    s_captured_sock = socket;
}

/* Task #119: do_handoff hands the S4a punch token over with the socket.
 * The probe's session phase feeds it to the production late_punch layer
 * exactly as netplay.c does. Size is STUN_PUNCH_TOKEN_LEN (stun.h) = 8;
 * mirrored literally here so this stub keeps zero src/ include deps. */
void Netplay_SetPunchToken(const unsigned char* token, bool valid) {
    if (token != NULL && valid) {
        memcpy(s_captured_token, token, sizeof(s_captured_token));
        s_captured_token_valid = true;
    } else {
        memset(s_captured_token, 0, sizeof(s_captured_token));
        s_captured_token_valid = false;
    }
}

void Netplay_SetSessionTeardownCallback(void (*cb)(void)) { s_teardown_cb = cb; }

struct NET_DatagramSocket* ProbeStub_Socket(void) { return s_captured_sock; }
const char* ProbeStub_RemoteIp(void) { return s_captured_ip; }
unsigned short ProbeStub_RemotePort(void) { return s_captured_port; }
const unsigned char* ProbeStub_PunchToken(bool* valid_out) {
    if (valid_out != NULL) {
        *valid_out = s_captured_token_valid;
    }
    return s_captured_token;
}

/* Task #119: production netplay.c fires this callback on session EXITING;
 * direct_p2p_on_teardown converts a NotifySessionFailed latch into the
 * terminal DIRECT_P2P_FAILED_HANDSHAKE park (direct_p2p.c). The probe's
 * session phase invokes it through here so a session deadline produces
 * the REAL parking state, via the REAL latch, not a probe-side imitation. */
void ProbeStub_InvokeTeardown(void) {
    if (s_teardown_cb != NULL) {
        s_teardown_cb();
    }
}

void Netplay_LogConnectEvent(const char* line) {
    if (line) fprintf(stderr, "[probe][connect] %s\n", line);
}

/* The MT sink and its initialiser. NOT optional and NOT new behaviour to
 * model -- production direct_p2p.c calls these from p2p_race,
 * host_thread_fn, DirectP2P_BeginHost and DirectP2P_BeginJoin, so without
 * them the probe does not LINK and the whole matrix is unrunnable.
 *
 * That is not hypothetical: it is the state this file was in when task
 * #121 found it. The stub stopped tracking direct_p2p.c the moment a
 * concurrent lane added the MT sink, and because nothing in the harness
 * builds the probe as part of a gate, the breakage sat here silently
 * while `run_matrix.sh` and `run_all.sh` kept exiting 0 on runs where no
 * cell had executed at all.
 *
 * Deliberately routed to the SAME stderr stream as the non-MT sink above
 * rather than to a file: the probe is a single-shot process whose stdout
 * is parsed as JSON by run_matrix.sh, and stderr is where every other
 * witness line already goes, so a matrix run's transcript keeps one
 * ordering for both sinks. Thread-safety comes from stderr being
 * unbuffered, which is what the production MT sink's callers assume of
 * whatever they are teed into. */
void Netplay_LogConnectEventMT(const char* line) {
    if (line) fprintf(stderr, "[probe][connect-mt] %s\n", line);
}

void Netplay_LogSinkInit(void) {
    fprintf(stderr, "[probe] Netplay_LogSinkInit\n");
}

/* Never called: even in --session mode the probe drives GekkoNet through
 * session_phase.c, not through the game loop. If it ever is called, say
 * so loudly rather than silently spinning. */
void Netplay_Run(void) {
    fprintf(stderr, "[probe] Netplay_Run called -- probe does not run a session\n");
}
