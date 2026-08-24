#ifndef THIRDSARM_WRAPPER_H
#define THIRDSARM_WRAPPER_H

int thirdsarm_wrapper_run(int argc, char *argv[]);

/*
 * Direct-P2P handoff between the OSD menu (menu.cpp) and the game runtime.
 *
 * Step 10 adds the OSD entry points: the user picks "Host Game" or enters
 * a peer code via the on-screen keyboard and picks "Join Game". The menu
 * calls one of these writer functions, which serializes the intent to a
 * short file at `kDirectP2PHandoffPath`, sets `g_direct_p2p_handoff_armed`,
 * then triggers a runtime restart (SIGTERM to the child + set
 * `g_wrapper_restart_requested`). Step 11 adds the exec-arg injection
 * that forwards `--direct-p2p-handoff <path>` to the relaunched game
 * when the arming flag is set.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* Serialize "mode=host\n" to the handoff file; arm + request restart. */
void direct_p2p_handoff_host(void);

/* Serialize "mode=join\ncode=<code>\n"; arm + request restart.
 * `code` must be a NUL-terminated string. Writer does not validate the
 * checksum — the game side (src/netplay/room_code.c) rejects bad codes. */
void direct_p2p_handoff_join(const char *code);

/* Recent-joins history helpers. See thirdsarm_wrapper.cpp for the storage
 * format; the menu uses these to render the "Recently Joined" submenu.
 *
 * load_recent_joins: fills codes_out (NUL-terminated, up to 24 chars incl. NUL —
 * the v3 room code is 18 chars without dashes; see src/netplay/room_code.h)
 * and optional epochs_out with up to max_entries entries (most-recent-first).
 * Returns the number of entries written.
 * save_recent_join: called from direct_p2p_handoff_join() after the handoff
 * file is committed. Dedupes, prepends, caps at 10. */
#define RECENT_JOIN_CODE_BUF 24
int  load_recent_joins(char codes_out[][RECENT_JOIN_CODE_BUF], long epochs_out[], int max_entries);
void save_recent_join(const char *code);

extern int g_direct_p2p_handoff_armed;

#ifdef __cplusplus
}
#endif

#endif
