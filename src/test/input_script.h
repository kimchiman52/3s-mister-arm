#if DEBUG

#ifndef INPUT_SCRIPT_H
#define INPUT_SCRIPT_H

#include "types.h"

#include <stdbool.h>

/*
 * Per-frame input-script player for the DEBUG test runner. Implements
 * Step H1 / H4 of docs/plan-frame-data-harness.md (sections 1.3, 1.6).
 *
 * Parses a line-oriented `.fdi` script (one directive per line; `#`
 * starts a comment that runs to the end of the line, including trailing
 * comments after a directive; blank/comment-only lines are ignored) and
 * plays it back frame-by-frame once training mode gameplay has started,
 * overwriting p1sw_buff/p2sw_buff at the same injection point
 * TestRunner_Prologue already uses. P1 is the attacker, P2 is the
 * training dummy.
 *
 * Directives:
 *   W <p1_word_hex> <p2_word_hex> <frames>
 *       Hold these SWK_* input words (sf33rd/AcrSDK/common/pad.h) for N
 *       frames. The only directive that advances time.
 *   L <label>
 *       Emit a "# F=<n> SCRIPT <label>" frame-trace annotation so a
 *       Python checker (Step H5) can pair the FINAL line(s) that follow
 *       with a corpus entry.
 *   P <p1_x> <p2_x>
 *       Teleport both players to absolute X positions.
 *   G <mode>
 *       Set the training dummy's (P2's) guard/stance mode. <mode> is one
 *       of:
 *         none   - no guard, standing (HIT/WHIFF corpus coverage)
 *         stand  - guard-all, standing (standing-block coverage)
 *         crouch - guard-all, crouching (crouching-block coverage)
 *       Takes effect immediately: pokes the training menu's guard/stance
 *       contents slots (surviving round resets) and control_pl_rno
 *       (immediate effect this frame) - see
 *       src/test/input_script.c's input_script_apply_guard_mode() for
 *       the exact fields. A sane default (`none`) is applied
 *       automatically the moment training gameplay starts, before the
 *       script's first directive runs, so scripts don't need to open
 *       with an explicit `G none` unless they want to document it.
 *       NOTE: when the resulting stance is STAND or CROUCH (i.e. every
 *       mode above), Control_Player_Tr() overwrites the dummy's input
 *       word every frame (menu.c:4188-4217), so this script's P2 W-word
 *       column is a dead field while a `G` mode is active - that's
 *       expected, not a bug in the script.
 *   Q
 *       End of script. After a ~120-frame grace period (so the last
 *       FINAL line has time to flush), requests a clean process exit
 *       via SDLApp_Exit() - exit code 0.
 *
 * Parse errors (missing/unreadable file, malformed line, unrecognized
 * directive, too many directives) are fatal: a diagnostic goes to
 * stderr and the process exits with a non-zero code immediately rather
 * than returning an error a caller might silently ignore. The same is
 * true if the script was loaded but the resolved --test-scene-preset
 * never boots training mode (see InputScript_RequireTrainingModePreset)
 * - both cases mean the script could never run, which a wrapper needs
 * to tell apart from a script that ran to completion (exit code 0).
 */

/* Parse the .fdi file at `path`. On success, the script is queued up and
 * InputScript_IsLoaded() becomes true. On any failure this does not
 * return: it prints a diagnostic to stderr and calls exit(). */
void InputScript_Load(const char* path);

/* True once InputScript_Load() has completed successfully. */
bool InputScript_IsLoaded(void);

/* Call once, after InputScript_Load(), passing whether the resolved
 * --test-scene-preset actually boots training mode. If a script was
 * loaded but the preset never reaches training mode,
 * training_mode_gameplay_started() would never go true and the script
 * would never get a chance to run - this fails loudly immediately
 * instead of hanging until an external timeout kills the process. */
void InputScript_RequireTrainingModePreset(bool scene_preset_uses_training_mode);

/* Advance the script by one frame. Must only be called once
 * training_mode_gameplay_started() is true. Writes the currently-held
 * input words (or 0) into *p1sw / *p2sw. No-op if no script is loaded. */
void InputScript_Tick(u16* p1sw, u16* p2sw);

/* True once the script's Q directive has finished its post-quit grace
 * period. InputScript_Tick() already requests the clean shutdown itself
 * (via SDLApp_Exit()) the moment this flips true; callers don't need to
 * poll this to trigger the exit, it's exposed for diagnostics/tests. */
bool InputScript_WantsExit(void);

#endif

#endif
