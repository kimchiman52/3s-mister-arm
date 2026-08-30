/* Task #119 — post-handoff GekkoNet session phase for p2p_probe.
 * See session_phase.c for what is real vs imitated. Only compiled when
 * the probe build found libGekkoNet.a (PROBE_HAS_SESSION). */
#ifndef PROBE_SESSION_PHASE_H
#define PROBE_SESSION_PHASE_H

#include <stdbool.h>

struct NET_DatagramSocket;

#define PROBE_SESSION_STARTED   0
#define PROBE_SESSION_DEADLINE  1
#define PROBE_SESSION_RIG_ERROR 2

typedef struct ProbeSessionReport {
    bool started;
    unsigned ms_to_session;
    int relearns; /* adapter retargets applied from late_punch verdicts */
} ProbeSessionReport;

int ProbeSession_Run(bool is_host, bool late_punch_on, unsigned grace_ms,
                     struct NET_DatagramSocket* sock,
                     const char* peer_ip, unsigned short peer_port,
                     const unsigned char* token, bool token_valid,
                     ProbeSessionReport* out);

#endif /* PROBE_SESSION_PHASE_H */
