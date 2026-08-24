#ifndef ARCADE_BALANCE_H
#define ARCADE_BALANCE_H

#include <stdbool.h>
#include <stdint.h>

/* Arcade (CPS3) vs PS2 balance AUTO-SELECTS at boot — there is no OSD
 * toggle. ArcadeBalance_Init resolves, in order:
 *   - test-runner mode          -> PS2 (harness determinism);
 *   - config `balance = ps2`    -> PS2 (config-file-only override);
 *   - CPS3 ROM verified AND all NUM_CHARS characters adapted -> Arcade;
 *   - anything else             -> PS2, with the reason logged, kept in
 *                                  ArcadeBalance_GetReason(), and written
 *                                  to <pref>/balance.status.
 * The resolution is fixed for the process lifetime. */
void ArcadeBalance_Init();
bool ArcadeBalance_IsEnabled();

/// "Arcade (CPS3)" or "PS2" — the string the OSD status line displays.
const char* ArcadeBalance_GetStatusText();

/// Why the session is PS2 ("" while arcade is active).
const char* ArcadeBalance_GetReason();

/// 64-bit digest of the fully-adapted arcade data (see
/// ArcadeCharData_ComputeDigest); 0 while PS2. Carried in the MIST netplay
/// handshake so peers with differing adapted data reject instead of desync.
uint64_t ArcadeBalance_GetDigest();

/* There is deliberately NO netplay force-off here anymore. The old
 * ArcadeBalance_ForceDisable was a process-lifetime latch that was never
 * cleared, so a menu-initiated netplay session left arcade balance off for
 * subsequent LOCAL play until relaunch. Netplay now requires the
 * verified-arcade state instead: every session entry path is gated by the
 * arm-time predicate Netplay_ArmAllowed() (src/netplay/netplay.c), and the
 * MIST handshake carries ArcadeBalance_GetDigest() so peers with differing
 * adapted data are rejected. */

#endif
