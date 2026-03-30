#include "netplay/matchmaking.h"

#include <stddef.h>

void Matchmaking_Start(const char* server_ip, int tcp_port, int udp_port) {
    (void)server_ip;
    (void)tcp_port;
    (void)udp_port;
}

void Matchmaking_Run() {
}

MatchmakingState Matchmaking_GetState() {
    return MATCHMAKING_IDLE;
}

const MatchResult* Matchmaking_GetResult() {
    return NULL;
}

NET_DatagramSocket* Matchmaking_GetSocket() {
    return NULL;
}

void Matchmaking_Reset() {
}
