/* Game-side symbols that src/netplay/direct_p2p.c references but which the probe
 * does not need: everything past the handoff boundary.
 *
 * The cascade under test ENDS at DIRECT_P2P_HANDOFF -- that is the moment the
 * orchestrator hands the punched socket to netplay.c and the rollback session
 * begins. Whether GekkoNet then syncs is a different question from whether the
 * two peers traversed their NATs, and it is the traversal we are measuring. So
 * these stubs record that the handoff happened and do nothing else.
 *
 * Deliberately NOT stubbed: anything in the traversal path. stun.c, rendezvous.c,
 * room_code.c, natpmp.c, upnp.c, connect_fail.c and direct_p2p.c itself are all
 * compiled from src/ unmodified.
 */
#include <stdbool.h>
#include <stdio.h>

struct NET_DatagramSocket;

static void (*s_teardown_cb)(void) = NULL;
static bool s_remote_ip_set = false;

void Netplay_SetParams(int player, const char* ip) {
    (void)player;
    s_remote_ip_set = (ip != NULL && ip[0] != '\0');
}

void Netplay_SetRemotePort(unsigned short port) { (void)port; }

bool Netplay_IsRemoteIpSet(void) { return s_remote_ip_set; }

/* Reaching here IS the success condition of the cascade. Log it so a run's
 * stdout independently corroborates the DIRECT_P2P_HANDOFF state the probe
 * reports -- two witnesses to the same event. */
void Netplay_BeginDirectP2P(void) {
    fprintf(stderr, "[probe] Netplay_BeginDirectP2P: handoff reached\n");
}

void Netplay_SetStunSocket(struct NET_DatagramSocket* socket) { (void)socket; }

void Netplay_SetSessionTeardownCallback(void (*cb)(void)) { s_teardown_cb = cb; }

void Netplay_LogConnectEvent(const char* line) {
    if (line) fprintf(stderr, "[probe][connect] %s\n", line);
}

/* Never called: the probe stops at the handoff and does not enter the game
 * loop. If it ever is called, say so loudly rather than silently spinning. */
void Netplay_Run(void) {
    fprintf(stderr, "[probe] Netplay_Run called -- probe does not run a session\n");
}
