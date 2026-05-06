#include "netplay/netplay.h"
#include "netplay/netplay_nav.h"

#include <string.h>

void Netplay_SetParams(int player, const char* ip) {
    (void)player;
    (void)ip;
}

bool Netplay_IsRemoteIpSet(void) {
    return false;
}

void NetplayNav_Arm(void) {
}

void NetplayNav_Tick(void) {
}

bool NetplayNav_IsActive(void) {
    return false;
}

void NetplayNav_Reset(void) {
}

void Netplay_BeginDirectP2P() {
}

void Netplay_TickDirectP2P() {
}

void Netplay_SetStunSocket(struct NET_DatagramSocket* socket) {
    (void)socket;
}

void Netplay_SetSessionTeardownCallback(void (*cb)(void)) {
    (void)cb;
}

void Netplay_SetMatchmakingParams(const char* server_ip, int server_port) {
    (void)server_ip;
    (void)server_port;
}

void Netplay_BeginMatchmaking() {
}

void Netplay_TickMatchmaking() {
}

bool Netplay_IsMatchmakingPending() {
    return false;
}

void Netplay_CancelMatchmaking() {
}

void Netplay_Run() {
}

NetplaySessionState Netplay_GetSessionState() {
    return NETPLAY_SESSION_IDLE;
}

void Netplay_HandleMenuExit() {
}

void Netplay_GetNetworkStats(NetworkStats* stats) {
    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
    }
}

bool Netplay_PollEvent(NetplayEvent* out) {
    (void)out;
    return false;
}

void Netplay_FlushDiagnostics(void) {
}
