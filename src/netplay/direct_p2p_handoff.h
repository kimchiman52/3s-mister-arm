/*
 * direct_p2p_handoff.h — Step 9 of docs/plan-stun-direct-p2p.md.
 *
 * Reads the one-shot handoff file written by the wrapper OSD (see
 * tools/mister-wrapper / vendor/Main_MiSTer/thirdsarm_wrapper.cpp) and
 * parses it into a mode + (port|peer_code) tuple. The file is a tiny
 * text blob with a handful of key=value lines:
 *
 *   mode=host|join
 *   port=<int>          # only if mode=host; 0 = OS-assigned
 *   peer_code=<string>  # only if mode=join
 *
 * Lifecycle is one-shot: main() reads the file, dispatches
 * DirectP2P_BeginHost/BeginJoin, then unlinks the file so a subsequent
 * launch without the wrapper does not re-trigger the flow with stale
 * data.
 *
 * This header is valid to include regardless of ENABLE_NETPLAY so the
 * main.c dispatch site stays tidy; the decode/dispatch body is gated
 * inside the .c TU.
 */

#ifndef NETPLAY_DIRECT_P2P_HANDOFF_H
#define NETPLAY_DIRECT_P2P_HANDOFF_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DIRECT_P2P_HANDOFF_MODE_NONE = 0,
    DIRECT_P2P_HANDOFF_MODE_HOST,
    DIRECT_P2P_HANDOFF_MODE_JOIN,
} DirectP2PHandoffMode;

typedef struct {
    DirectP2PHandoffMode mode;
    int port;             /* valid when mode == HOST; 0 means OS-assigned */
    char peer_code[64];   /* valid when mode == JOIN; NUL-terminated */
} DirectP2PHandoff;

/*
 * Parse the handoff file at `path` into `*out`. Returns true on success
 * (mode recognized, per-mode required fields present). Returns false and
 * leaves *out zeroed if the file is missing, unreadable, too large
 * (>4KB), or malformed (missing/unknown mode, join with no peer_code).
 *
 * Prints a one-line diagnostic to stderr on parse errors. A missing file
 * is NOT an error — the caller should treat false-with-missing-file as a
 * fall-through to the normal title flow. Callers can distinguish by
 * their own access(path, F_OK) check if they care; in practice both
 * paths go to the same no-op.
 */
bool DirectP2PHandoff_ReadFile(const char* path, DirectP2PHandoff* out);

/*
 * Convenience: unlink the handoff file after a successful read. Prints a
 * one-line warning on failure but does not return an error — stale-file
 * UX is acceptable degradation per the plan (see docs/plan-stun-direct-p2p.md
 * §9 "What to do if it fails").
 */
void DirectP2PHandoff_Consume(const char* path);

/*
 * Returns the configured default handoff path (the wrapper's canonical
 * tmpfs drop location). Read from CFG_KEY_NETPLAY_DIRECT_P2P_HANDOFF_PATH,
 * which defaults to "/tmp/3s-arm-netplay.handoff" (see
 * src/port/config/config.c). May return NULL if the config system is
 * not yet initialized. Safe to call when ENABLE_NETPLAY is off (returns
 * NULL).
 */
const char* DirectP2PHandoff_ConfigDefaultPath(void);

/*
 * Non-destructive check — is there a file at `path`? Caller uses this
 * to short-circuit the config-default fallback without emitting a stat
 * error when the file simply doesn't exist.
 */
bool DirectP2PHandoff_FileExists(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_DIRECT_P2P_HANDOFF_H */
