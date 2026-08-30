/* Task #119: the stub's do_handoff captures + hooks, consumed by
 * session_phase.c (and declared for the stub itself). See the .c header
 * for why the captures exist. */
#ifndef NETPLAY_PROBE_STUB_H
#define NETPLAY_PROBE_STUB_H

#include <stdbool.h>

struct NET_DatagramSocket;

/* Prototypes for the game-side symbols the stub provides, so the stub is
 * compiled against the same signatures direct_p2p.c calls (netplay.h is
 * NOT included here on purpose — the stub must keep zero src/ include
 * deps, and a signature drift then fails the LINK of the probe, which is
 * the loud failure this harness wants). */
void Netplay_SetParams(int player, const char* ip);
void Netplay_SetRemotePort(unsigned short port);
bool Netplay_IsRemoteIpSet(void);
void Netplay_BeginDirectP2P(void);
void Netplay_SetStunSocket(struct NET_DatagramSocket* socket);
void Netplay_SetPunchToken(const unsigned char* token, bool valid);
void Netplay_SetSessionTeardownCallback(void (*cb)(void));
void Netplay_LogConnectEvent(const char* line);
void Netplay_LogConnectEventMT(const char* line);
void Netplay_LogSinkInit(void);
void Netplay_Run(void);

/* Captures (valid after DIRECT_P2P_HANDOFF; the socket is BORROWED from
 * direct_p2p/netplay ownership rules — the probe never closes it). */
struct NET_DatagramSocket* ProbeStub_Socket(void);
const char* ProbeStub_RemoteIp(void);
unsigned short ProbeStub_RemotePort(void);
const unsigned char* ProbeStub_PunchToken(bool* valid_out);
void ProbeStub_InvokeTeardown(void);

#endif /* NETPLAY_PROBE_STUB_H */
