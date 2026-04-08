# Implementation Plan: CONF_STR Menu Migration

Migrate the 3SX MiSTer core from a custom OSD menu to the standard MiSTer
CONF_STR-driven system menu. This removes ~700 lines of custom menu code and
gives button mapping, video processing, audio filters, save/load settings, and
three-page navigation for free.

See `docs/plan-confstr-menu-migration.md` for the high-level design.

---

## Build System Context

Understanding the build pipeline is critical for knowing which files to edit:

- **FPGA core build** (`tools/mister-wrapper/build-core.sh`):
  1. Copies `vendor/Menu_MiSTer/` to `build/mister-wrapper-core/src/`
  2. Renames `menu.sv` -> `3SX.sv`, `menu.qpf` -> `3SX.qpf`, `menu.qsf` -> `3SX.qsf`
  3. Ruby patcher replaces `"MENU;UART31250,MIDI;"` with `"3SX;;"` in the `.sv` file
  4. Runs Quartus synthesis

- **HPS wrapper build** (`tools/mister-wrapper/build-hps.sh`):
  1. Clones upstream Main_MiSTer at pinned commit into `build/mister-wrapper-hps/src/`
  2. Copies overlay files listed in `tools/mister-wrapper/main-mister-overlay.files`
     from `vendor/Main_MiSTer/` over the upstream clone
  3. Applies `tools/mister-wrapper/main-mister-full-menu.patch` to `menu.cpp`/`menu.h`
  4. Builds with `tools/mister-wrapper/Makefile.full.3sx`

Therefore:
- **Edit `vendor/Menu_MiSTer/menu.sv`** for CONF_STR changes (becomes `3SX.sv` at build time)
- **Edit `vendor/Main_MiSTer/threesx_wrapper.cpp`** for ARM-side changes (overlay file)
- **Edit `vendor/Main_MiSTer/threesx_wrapper.h`** for header changes (overlay file)
- **Edit `tools/mister-wrapper/main-mister-full-menu.patch`** for menu.cpp/menu.h changes
- The `build/` directory is generated output -- never edit directly

## Status Bit Layout (FPGA)

Bits 0-9 are already used by the FPGA core:

| Bits   | Usage                    | Source                    |
|--------|--------------------------|---------------------------|
| [0]    | Reset                    | MiSTer framework          |
| [3:1]  | Reserved (FB terminal)   | MiSTer framework (menu bg)|
| [4]    | PAL                      | `wire PAL = status[4]`    |
| [5]    | FB (framebuffer)         | `wire FB = status[5]`     |
| [8:6]  | LED control              | `wire [2:0] led = status[8:6]` |
| [9]    | NATIVE_VID               | `wire NATIVE_VID = status[9]` |

Bits 1-3 are reserved by the MiSTer framework for menu background selection
(see `user_io.cpp` line 1533: `video_menu_bg(user_io_status_get("[3:1]"))`).
The 3SX core does not use them but they must not be repurposed.

Bits 10+ are safe for menu options. The FPGA core does not read them.

---

## Step 1: Expand CONF_STR with Menu Options

### Title
Add O/R option entries to the CONF_STR in `vendor/Menu_MiSTer/menu.sv`.

### Why it matters
The CONF_STR string defines what appears in the standard MiSTer core menu (center
page). Without it, there are no settings to show. This is the foundation for all
subsequent steps. It requires a Quartus FPGA rebuild, which takes ~45 minutes,
so it should be started early.

### Files to read before implementing
- `/Users/sb/Developer/3sx-mister/vendor/Menu_MiSTer/menu.sv` (lines 224-231)
- `/Users/sb/Developer/3sx-mister/tools/mister-wrapper/build-core.sh` (lines 121-155
  for the Ruby patcher -- confirms `"MENU;UART31250,MIDI;"` becomes `"3SX;;"`)

### Files to modify
**`vendor/Menu_MiSTer/menu.sv`** -- Replace lines 225-231:

```verilog
localparam CONF_STR = {
    "MENU;UART31250,MIDI;",
    "J1,LP,MP,HP,LK,MK,HK,Select,Start;",
    "jn,Y,X,L,B,A,R,Select,Start;",
    "-;",
    "O[11:10],Show FPS,Off,FPS,Debug;",
    "O[13:12],Scale Mode,Auto,Native,Nearest;",
    "-;",
    "O[14],SA Activation,Full,Cached BG;",
    "O[15],SA Ghost Res,Full,Half;",
    "O[18:16],SA Ghost Count,0,1,2,3,4;",
    "-;",
    "O[20:19],Overclock,Stock,1000MHz,1200MHz;",
    "-;",
    "O[21],Reset to Default,No,Yes;",
    "O[22],Restart Game,No,Yes;",
    "-;",
    "V,v",`BUILD_DATE
};
```

**CRITICAL: Why O-type instead of T-type for Reset/Restart (P-1.1 fix):**
`HandleUI()` in `menu.cpp` (lines 2515-2516) pulses T/R trigger bits within a
single call: `user_io_status_set(opt, 1, ex)` immediately followed by
`user_io_status_set(opt, 0, ex)`. Since the wrapper's `poll_status_changes()`
runs in a separate iteration of the main loop (after `HandleUI()` returns), it
would **always see 0** for T-type bits -- the trigger would never fire.

The fix: use O-type toggle bits for Reset to Default ([21]) and Restart Game
([22]). The poller detects the transition from 0 to 1, performs the action, and
then clears the bit back to 0 via `user_io_status_set`. Quit to MiSTer is
removed entirely — the system page's "Reboot" provides the same functionality.

**SA Activation option order (P-1.2 fix):** The CONF_STR option order must match
the enum values in `threesx_wrapper.cpp`. `kSuperEffectQualityFull=0` and
`kSuperEffectQualityCachedBg=1`, so the CONF_STR must list "Full" first (value 0)
and "Cached BG" second (value 1): `"O[14],SA Activation,Full,Cached BG;"`. The
original plan had them reversed, which would have caused value 0 from the menu
("Cached BG" in old order) to map to `kSuperEffectQualityFull` in the enum.

**Note:** The `"MENU;UART31250,MIDI;"` token gets replaced with `"3SX;;"` by the
Ruby patcher during `build-core.sh --prepare-source`. Only the first line changes;
all O/R lines pass through unchanged.

### Success criteria
1. `grep -c 'O\[' vendor/Menu_MiSTer/menu.sv` returns 8 (eight O options, including Reset/Restart)
2. `grep -c 'T\[' vendor/Menu_MiSTer/menu.sv` returns 0 (no T-type triggers)
3. `grep -c 'R\[' vendor/Menu_MiSTer/menu.sv` returns 0 (no R-type triggers — Quit handled by system Reboot)
4. `tools/mister-wrapper/build-core.sh --prepare-source` succeeds and the patched
   `build/mister-wrapper-core/src/3SX.sv` contains `"3SX;;"` (not `"MENU;..."`)
   AND contains all O/R lines
5. Quartus build produces `3SX.rbf` (kick off with `--fast` in colima VM)

### Dependencies
None. This is the first step.

### What NOT to do
- Do not modify `build/mister-wrapper-core/src/3SX.sv` directly (it is generated)
- Do not add any status bit usage to the Verilog -- bits 10+ are ARM-only
- Do not change the `hps_io` instantiation or `status_menumask` wiring

### What to do if it fails
- If prepare-source fails: check that the `"MENU;UART31250,MIDI;"` token still
  exists exactly as written (the Ruby patcher does a literal string match)
- If Quartus fails: likely unrelated to CONF_STR changes (it is just a string
  constant); check Quartus logs for timing/fitting errors

---

## Step 2: Add Status-Change Poller to threesx_wrapper.cpp

### Title
Add a `poll_status_changes()` function that reads status bits and applies
runtime settings via signals and config-file writes.

### Why it matters
This is the ARM-side bridge between the CONF_STR menu (which writes to `status`
bits) and the game runtime (which responds to POSIX signals and config files).
This must exist before the custom menu is removed.

### Files to read before implementing
- `/Users/sb/Developer/3sx-mister/vendor/Main_MiSTer/threesx_wrapper.cpp`
  (lines 1-140 for constants/enums, lines 373-1272 for `read_runtime_*` and
  `write_runtime_*` helpers, lines 1700-1750 for `wait_for_child`)
- `/Users/sb/Developer/3sx-mister/vendor/Main_MiSTer/threesx_wrapper.h`
- `/Users/sb/Developer/3sx-mister/build/mister-wrapper-hps/src/user_io.h`
  (lines 185-186 for `user_io_status_get`/`user_io_status_set` signatures)
- `/Users/sb/Developer/3sx-mister/build/mister-wrapper-hps/src/user_io.cpp`
  (lines 524-571 for implementation -- reads from `cur_status` array, no SPI)

### Files to modify
**`vendor/Main_MiSTer/threesx_wrapper.cpp`** -- Add a new function in the
anonymous namespace (near line 1536, after `service_wrapper_menu`):

```cpp
void poll_status_changes(pid_t child)
{
    // Cache previous status bits for change detection
    static uint32_t prev_fps = 0xFFFFFFFF;
    static uint32_t prev_sa_activation = 0xFFFFFFFF;
    static uint32_t prev_ghost_res = 0xFFFFFFFF;
    static uint32_t prev_ghost_count = 0xFFFFFFFF;
    static uint32_t prev_arm_clock = 0xFFFFFFFF;
    static uint32_t prev_scale_mode = 0xFFFFFFFF;

    // --- Option bits: detect changes and apply ---

    uint32_t fps = user_io_status_get("[11:10]");
    if (fps != prev_fps) {
        prev_fps = fps;
        // CONF_STR: 0=Off, 1=FPS, 2=Debug (matches kFpsOverlay* enums)
        int target = (int)fps;
        if (target != g_wrapper_fps_mode) {
            write_runtime_fps_default(target);
            // FPS signal is a toggle (cycles Off->FPS->Debug->Off).
            // Compute the number of toggles needed to reach the target.
            int toggles = (target - g_wrapper_fps_mode + kFpsOverlayModeCount)
                          % kFpsOverlayModeCount;
            g_wrapper_fps_mode = target;
            for (int i = 0; i < toggles; i++)
                kill(child, kRuntimeFpsToggleSignal);
        }
    }

    uint32_t sa_activation = user_io_status_get("[14]");
    if (sa_activation != prev_sa_activation) {
        prev_sa_activation = sa_activation;
        // Enum values match CONF_STR option order (0=Full, 1=CachedBg)
        int target = (int)sa_activation;
        if (target != g_wrapper_super_effect_quality) {
            write_runtime_super_effect_quality_default(target);
            int cycles = (target - g_wrapper_super_effect_quality
                          + kSuperEffectQualityMenuCount)
                         % kSuperEffectQualityMenuCount;
            g_wrapper_super_effect_quality = target;
            for (int i = 0; i < cycles; i++)
                kill(child, kRuntimeSuperEffectQualityCycleSignal);
        }
    }

    // [Similar delta-cycle blocks for Ghost Res [15], Ghost Count [18:16]]

    uint32_t arm_clock = user_io_status_get("[20:19]");
    if (arm_clock != prev_arm_clock) {
        prev_arm_clock = arm_clock;
        int target = (int)arm_clock;
        if (target != g_wrapper_arm_clock) {
            write_runtime_arm_clock_default(target);
            // Overclock defers to restart -- update the pending value only.
            // g_wrapper_arm_clock_active is applied in the restart loop.
            g_wrapper_arm_clock = target;
            // Do NOT send kRuntimeArmClockCycleSignal here.
        }
    }

    uint32_t scale_mode = user_io_status_get("[13:12]");
    if (scale_mode != prev_scale_mode) {
        prev_scale_mode = scale_mode;
        // Scale mode is startup-only. Write config, no signal.
        write_runtime_scale_mode_default((int)scale_mode);
    }

    // --- O-type action bits (Reset/Restart) ---
    // These use O-type toggles instead of T/R triggers because HandleUI()
    // pulses T/R bits within a single call (set(1) then set(0)), making
    // them invisible to the poller. O-type bits stay set until we clear them.

    if (user_io_status_get("[21]")) {
        // Reset to Default: set option bits to defaults, clear the trigger
        user_io_status_set("[21]", 0);
        user_io_status_set("[11:10]", 0); // FPS off
        user_io_status_set("[13:12]", 0); // Scale auto
        user_io_status_set("[14]", kSuperEffectQualityCachedBg); // SA default=CachedBg
        user_io_status_set("[15]", 0);    // Ghost Res = Full (enum 0)
        user_io_status_set("[18:16]", kGhostCount4); // Ghost Count default=4
        user_io_status_set("[20:19]", 0); // Overclock = Stock
        // Force prev_ values to 0xFFFFFFFF so next poll iteration detects
        // the change and sends the appropriate cycle signals
        prev_fps = 0xFFFFFFFF;
        prev_sa_activation = 0xFFFFFFFF;
        prev_ghost_res = 0xFFFFFFFF;
        prev_ghost_count = 0xFFFFFFFF;
        prev_arm_clock = 0xFFFFFFFF;
        prev_scale_mode = 0xFFFFFFFF;
    }

    if (user_io_status_get("[22]")) {
        // Restart Game: clear the trigger, then restart
        user_io_status_set("[22]", 0);
        g_wrapper_restart_requested = 1;
        kill(child, SIGTERM);
    }

    // Quit to MiSTer removed — system page "Reboot" handles it.
}
```

**Important design details:**

1. **FPS overlay has 3 modes (Off/FPS/Debug).** The CONF_STR uses 2 bits
   [11:10] with values 0=Off, 1=FPS, 2=Debug, matching the `kFpsOverlay*`
   enum values directly. All three modes are accessible from the menu.

2. **Scale Mode ([13:12]) is startup-only.** The game reads it from the config
   file at launch. Changing it mid-game has no effect. The poller should write
   the config file (`write_runtime_scale_mode_default`) but NOT send a signal.
   It takes effect on the next Restart.

3. **All runtime settings use cycle-signals**, not set-to-value signals. The
   FPS toggle signal (`kRuntimeFpsToggleSignal`) cycles Off->FPS->Debug->Off.
   The SA/ghost/overclock cycle signals advance by one step. The poller must
   compute the modular delta between the old and new enum value and send that
   many signals. For example, if Ghost Count changes from 1 to 3, send 2 cycle
   signals. The "Reset to Default" action invalidates all `prev_` cache values
   so the next poll iteration computes deltas naturally.

4. **Overclock defers to restart.** The current code already defers overclock
   changes to the next game restart. The poller should write the config and
   update `g_wrapper_arm_clock` but NOT send `kRuntimeArmClockCycleSignal`.
   `g_wrapper_arm_clock_active` is applied in the restart loop (line 2069).

5. **Reset to Default ([21]) and Restart Game ([22]) use O-type toggles (not
   T/R triggers).** `HandleUI()` in `menu.cpp` (lines 2515-2516) pulses T/R
   bits within one call: `user_io_status_set(opt, 1)` immediately followed
   by `user_io_status_set(opt, 0)`. The poller runs in a separate main-loop
   iteration and would always see 0. O-type bits persist until the poller
   explicitly clears them via `user_io_status_set("[bit]", 0)`.

6. **Quit to MiSTer removed.** The system page "Reboot" provides the same
   functionality. No custom CONF_STR entry or poller handling needed.

7. **Reset to Default defaults must match the custom menu defaults.**
   The current custom menu (lines 1477-1512) resets to: FPS=Off,
   SA Activation=CachedBg (enum 1), Ghost Res=Full (enum 0),
   Ghost Count=4 (enum 4), Overclock=Stock (enum 0). The poller must use
   these same values, not zero for all bits.

### Success criteria
1. Function compiles without errors (HPS build succeeds)
2. `grep -c 'poll_status_changes' vendor/Main_MiSTer/threesx_wrapper.cpp` returns >= 2
   (definition + call site -- call added in Step 4)
3. The function does NOT call `threesx_wrapper_menu_key_take()` or any OSD draw functions

### Dependencies
- Step 1 (CONF_STR must define the bit layout this function reads)

### What NOT to do
- Do not call the new function yet (that happens in Step 4 when the old menu is removed)
- Do not remove any existing code in this step
- Do not add `#include "support/arcade/mra_loader.h"` dependencies

### What to do if it fails
- If `user_io_status_get` is not found at link time: verify that `user_io.cpp` is
  in the overlay files list (`tools/mister-wrapper/main-mister-overlay.files` --
  it is: confirmed `user_io.cpp` is listed)
- If status bits read as zero: check that CONF_STR was rebuilt and the correct
  `.rbf` is loaded on the MiSTer

---

## Step 3: Add Initial Status Restoration on Game Launch

### Title
Seed status bits 10-22 from the persisted runtime config when the wrapper starts,
so the MiSTer menu shows the correct current values.

### Why it matters
The MiSTer framework loads `cur_status` from `3SX.CFG` on core init (see
`user_io.cpp` lines 1510-1511: `memset(cur_status, 0, ...);
FileLoadConfig(name, cur_status, ...)`). If a `.cfg` file exists with
previously-saved status bits, MiSTer restores them automatically. However, the
game's runtime config file (`/media/fat/games/3sx/config`) is the authoritative
source for settings like FPS mode, SA quality, etc. These two sources can
diverge (e.g., user edits the config file manually, or the `.cfg` file does
not exist yet).

**Design decision: game config is authoritative.** The seeding step
overwrites `cur_status` bits 10-22 from the game config. This ensures the menu
always reflects the actual runtime settings. The MiSTer `.cfg` file still
stores these bits on "Save Settings", but the game config takes priority on
load.

### Files to read before implementing
- `/Users/sb/Developer/3sx-mister/vendor/Main_MiSTer/threesx_wrapper.cpp`
  (lines 1753-1820 for `threesx_wrapper_run` initialization, lines 373-1272
  for `read_runtime_*_default` and `write_runtime_*_default` helpers)
- `/Users/sb/Developer/3sx-mister/build/mister-wrapper-hps/src/user_io.h`
  (line 186 for `user_io_status_set` signature)
- `/Users/sb/Developer/3sx-mister/build/mister-wrapper-hps/src/user_io.cpp`
  (lines 1506-1522 for the config restore path that loads `cur_status` from
  `.cfg` -- seeding must happen AFTER this)

### Files to modify
**`vendor/Main_MiSTer/threesx_wrapper.cpp`** -- In `threesx_wrapper_run()`,
**after** the `g_wrapper_used_full_user_io_init` block (after line 1794), add
status bit seeding. This placement is critical: it must be AFTER `user_io_init`
has completed (which loads `cur_status` from `.cfg`) so the seeding overwrites
the MiSTer-restored values with the authoritative game config values.

```cpp
// Seed CONF_STR status bits from persisted game config so the MiSTer
// menu reflects the actual runtime settings. This overwrites any values
// loaded from 3SX.CFG by user_io_init -- the game config is authoritative.
user_io_status_set("[11:10]", (uint32_t)g_wrapper_fps_mode);
user_io_status_set("[13:12]", (uint32_t)read_runtime_scale_mode_default());
user_io_status_set("[14]", (uint32_t)g_wrapper_super_effect_quality);
user_io_status_set("[15]", (uint32_t)g_wrapper_ghost_resolution);
user_io_status_set("[18:16]", (uint32_t)g_wrapper_ghost_count);
user_io_status_set("[20:19]", (uint32_t)g_wrapper_arm_clock);
```

**Important placement note (P-2.2 fix):** Do NOT place the seeding at line
1764 (right after `read_runtime_*_default()` calls). At that point,
`user_io_init` has not yet run. The seeding must go AFTER the
`g_wrapper_used_full_user_io_init` block (after line 1794) where the FPGA
and SPI are known to be ready.

**SA Activation seeding (P-1.2 fix):** The CONF_STR option order now matches
the enum values directly (0=Full, 1=CachedBg), so the seeding can use the
enum value as-is: `(uint32_t)g_wrapper_super_effect_quality`. No inversion
needed.

Also add seeding inside the restart loop (around line 2067) so that after a
restart, the menu still reflects the correct values. Also reset the poller's
`prev_` static cache by calling a `poll_status_reset()` helper (or by placing
the restart seeding before the next `poll_status_changes` call, which will
detect the freshly-seeded values).

### Success criteria
1. HPS build succeeds
2. On the MiSTer, open the core menu (F12) immediately after launch -- all
   option values should match what is in `/media/fat/games/3sx/config`
3. Change a setting, restart, open menu -- the setting persists in the menu

### Dependencies
- Step 1 (CONF_STR bit ranges must match)
- Step 2 (poller must exist so changes from the menu are applied)

### What NOT to do
- Do not seed bits 0-9 (these are FPGA core bits managed elsewhere)
- Do not call `user_io_status_set` before FPGA initialization

### What to do if it fails
- If values appear wrong: print `user_io_status_get` values in the wrapper log
  after seeding to verify the round-trip
- If seeding causes FPGA glitches: ensure bits 0-9 are not touched (use
  specific bit-range strings, not full 32-bit writes)

---

## Step 4: Remove Custom Menu Code and Wire Up Poller

### Title
Delete the custom menu rendering and key-handling code, and replace the
`service_wrapper_menu()` call with `poll_status_changes()`.

### Why it matters
This is the core migration step. The custom menu is replaced by the standard
MiSTer menu. The wrapper's main loop no longer intercepts keys -- it just polls
status bits for changes.

### Files to read before implementing
- `/Users/sb/Developer/3sx-mister/vendor/Main_MiSTer/threesx_wrapper.cpp`
  (full file -- you need to know every reference to the removed code)
- `/Users/sb/Developer/3sx-mister/vendor/Main_MiSTer/threesx_wrapper.h`

### Files to modify

**`vendor/Main_MiSTer/threesx_wrapper.cpp`** -- Remove the following:

1. **Remove `#include "support/arcade/mra_loader.h"`** (line 34). This was only
   needed for `mgl_get()->done = 1` in the Define Buttons passthrough. The
   standard menu handles button mapping natively.

2. **Remove the `WrapperMenuItem` enum** (lines 65-78).

3. **Remove global state variables** (lines 130-132):
   - `g_wrapper_menu_visible`
   - `g_wrapper_menu_selected`
   - `g_wrapper_menu_passthrough`

4. **Remove `format_wrapper_value_line()`** (lines 1274-1285).

5. **Remove `draw_wrapper_menu()`** (lines 1287-1343).

6. **Remove `service_wrapper_menu()`** (lines 1345-1536).

7. **Update `wait_for_child()`** (lines 1707-1749): Replace the call to
   `service_wrapper_menu(child)` (line 1739) with `poll_status_changes(child)`.
   Remove the `HandleUI(); OsdUpdate();` calls that follow -- wait, actually
   **keep** `HandleUI()` and `OsdUpdate()`. They are needed for the standard
   MiSTer menu to function. The new loop body should be:

   ```cpp
   poll_status_changes(child);
   HandleUI();
   OsdUpdate();
   ```

8. **Update `threesx_wrapper_run()`** (lines 1756-1757): Remove the lines that
   set `g_wrapper_menu_visible = 0` and `g_wrapper_menu_selected = kMenuResume`.

9. **Update restart handling** (line 2067-2068): Remove `g_wrapper_menu_visible = 0`
   and `g_wrapper_menu_selected = kMenuResume`.

10. **Remove `disable_wrapper_osd()`** if no longer called. Check all call sites:
    - Line 1170: definition
    - Lines 1388, 1519, 1527, 1533: inside `service_wrapper_menu` (being removed)
    - Line 1637: inside `show_wrapper_message` flow
    - Line 1825: in `threesx_wrapper_run` startup

    The calls at 1637 and 1825 still exist outside the menu code. **Keep
    `disable_wrapper_osd()` but simplify it:** remove the
    `threesx_wrapper_menu_set_visible(0)` call from it since that function is
    being removed. Keep `OsdMenuCtl(0); OsdDisable(); OsdUpdate();`.

11. **Remove `show_wrapper_message()` and `split_message_line()`** only if they
    are no longer used. Check: `show_wrapper_message` is called at line 1635
    in the error path. **Keep it** -- but remove the
    `threesx_wrapper_menu_set_visible(0)` reference from `disable_wrapper_osd`.

12. **Keep all `read_runtime_*` and `write_runtime_*` helper functions.** They
    are still used by the status poller and initial seeding.

**`vendor/Main_MiSTer/threesx_wrapper.h`** -- Remove the two custom menu
function declarations:

```cpp
// Remove these two lines:
unsigned int threesx_wrapper_menu_key_take();
void threesx_wrapper_menu_set_visible(int visible);
```

The header should become:
```cpp
#ifndef THREESX_WRAPPER_H
#define THREESX_WRAPPER_H

int threesx_wrapper_run(int argc, char *argv[]);

#endif
```

### Success criteria
1. `grep -c 'service_wrapper_menu' vendor/Main_MiSTer/threesx_wrapper.cpp` returns 0
2. `grep -c 'draw_wrapper_menu' vendor/Main_MiSTer/threesx_wrapper.cpp` returns 0
3. `grep -c 'threesx_wrapper_menu_key_take' vendor/Main_MiSTer/threesx_wrapper.cpp` returns 0
4. `grep -c 'g_wrapper_menu_visible' vendor/Main_MiSTer/threesx_wrapper.cpp` returns 0
5. `grep -c 'g_wrapper_menu_passthrough' vendor/Main_MiSTer/threesx_wrapper.cpp` returns 0
6. `grep -c 'mra_loader' vendor/Main_MiSTer/threesx_wrapper.cpp` returns 0
7. `grep -c 'poll_status_changes' vendor/Main_MiSTer/threesx_wrapper.cpp` returns >= 2
8. HPS build succeeds

### Dependencies
- Step 2 (poller must exist to replace the custom menu)
- Step 3 (initial seeding must exist so menu shows correct values)

### What NOT to do
- Do not remove `HandleUI()` or `OsdUpdate()` from the main loop -- the standard
  menu depends on them
- Do not remove `read_runtime_*` or `write_runtime_*` helper functions
- Do not remove `show_wrapper_message()` or `disable_wrapper_osd()` (still used)
- Do not remove the `kRuntime*Signal` constants (`kRuntimeFpsToggleSignal`,
  `kRuntimeSuperEffectQualityCycleSignal`, etc.) -- they are used by the poller.
  These are separate from the `WrapperMenuItem` enum being removed.
- Do not touch `input_poll()`, `input_get_joy_mask()`, or the shared memory code

### What to do if it fails
- If HPS build fails with undefined references to `threesx_wrapper_menu_key_take`
  or `threesx_wrapper_menu_set_visible`: check that the patch file (Step 5) has
  also been updated, or that the stubs file references have been handled
- Compile errors are likely from missed reference removal -- grep for the
  removed function names and fix

---

## Step 5: Update the menu.cpp/menu.h Patch

### Title
Reduce the menu patch to a single MENU_SYSTEM1 hunk. Remove all custom menu
additions and revert the MENU_JOYDIGMAP4 change.

### Why it matters
The patch file (`tools/mister-wrapper/main-mister-full-menu.patch`) currently adds
three custom menu functions to `menu.cpp` and modifies `menu_present()` and the
JOYDIGMAP/SYSTEM1 state transitions. With the standard menu:
- `threesx_wrapper_menu_key_take()` and `threesx_wrapper_menu_set_visible()` are
  no longer called by anyone
- `wrapper_menu_visible` is no longer needed in `menu_present()`
- The JOYDIGMAP4 exit should go back to `MENU_COMMON1` (standard behavior)
- The MENU_SYSTEM1 `is_menu()` guard **MUST be kept** -- reverting it would
  hide the system page for 3SX (see P-1.3 analysis)

### Files to read before implementing
- `/Users/sb/Developer/3sx-mister/tools/mister-wrapper/main-mister-full-menu.patch`
  (the complete patch -- 80 lines)
- `/Users/sb/Developer/3sx-mister/build/mister-wrapper-hps/src/menu.cpp`
  (lines 4280-4303 for JOYDIGMAP4, lines 6622-6627 for MENU_SYSTEM1,
  lines 7856-7860 for `menu_present()`)
- `/Users/sb/Developer/3sx-mister/build/mister-wrapper-hps/src/menu.h`
  (lines 10-11 for the added declarations)

### Files to modify
**`tools/mister-wrapper/main-mister-full-menu.patch`** -- Rewrite the patch to
remove all custom menu additions. The new patch should be either:

**Minimal patch (P-1.3 fix).** The patch cannot be emptied entirely because the
MENU_SYSTEM1 hunk must be KEPT. Analysis of each hunk:

1. **menu.h: `threesx_wrapper_menu_key_take` and `threesx_wrapper_menu_set_visible`
   declarations** -- REMOVE (no longer needed)

2. **menu.cpp: `wrapper_menu_visible` variable, `menu_key_get` forward decl,
   `threesx_wrapper_menu_key_take()` and `threesx_wrapper_menu_set_visible()`
   implementations** -- REMOVE

3. **menu.cpp: MENU_JOYDIGMAP4 exit to `MENU_NONE1` instead of `MENU_COMMON1`**
   -- REVERT (standard menu should return to MENU_COMMON1 after button mapping)

4. **menu.cpp: MENU_SYSTEM1 `is_menu()` instead of `video_fb_state()`** --
   **KEEP.** This is the critical hunk. `is_menu_like_fb()` returns true for
   3SX (see `user_io.cpp` line 222: `is_menu() || !strcasecmp(orig_name, "3SX")`).
   `video_fb_state()` calls `is_menu_like_fb()` and returns `fb_enabled && !fb_num`.
   When the framebuffer is enabled (the common case for 3SX), `video_fb_state()`
   returns true, and the upstream guard `if (video_fb_state()) { menustate =
   MENU_NONE1; break; }` would skip the system page entirely. The patch changes
   this to `is_menu()`, which returns false for 3SX, allowing system page access.
   **Reverting this hunk would hide the system page for all non-native-video 3SX
   users.**

5. **menu.cpp: `menu_present()` includes `wrapper_menu_visible`** -- REVERT
   (no custom menu visibility to track)

**Recommended approach:** Reduce the patch to a single hunk: the MENU_SYSTEM1
`video_fb_state()` -> `is_menu()` change. Remove all other hunks. The patch
file remains non-empty, so `build-hps.sh` line 92 (`git apply`) works without
modification.

**Audit of all `is_menu_like_fb()` call sites (P-2.6):** Before finalizing,
verify that `is_menu_like_fb()` is not used in other menu-related guards that
could similarly block 3SX functionality. Known call sites in `video.cpp`:
- Line 3307: `video_fb_enable` -- controls FB buffer selection, unrelated to menu
- Line 3365: `user_io_status_set("[8:5]", ...)` -- LED/FB status bits, unrelated
- Line 3380: `video_fb_state()` -- the function we are patching around
Add a note in the test procedure (Step 7) to verify the system page is accessible.

### Success criteria
1. `tools/mister-wrapper/build-hps.sh --prepare-source` succeeds
2. The generated `build/mister-wrapper-hps/src/menu.cpp` does NOT contain
   `threesx_wrapper_menu_key_take` or `wrapper_menu_visible`
3. The generated `build/mister-wrapper-hps/src/menu.h` does NOT contain
   `threesx_wrapper_menu_key_take`
4. `grep 'MENU_JOYDIGMAP4' build/mister-wrapper-hps/src/menu.cpp` followed by
   checking exit states -- they should go to `MENU_COMMON1`, not `MENU_NONE1`
5. The generated `build/mister-wrapper-hps/src/menu.cpp` contains `is_menu()`
   (not `video_fb_state()`) in the MENU_SYSTEM1 guard
6. HPS build succeeds

### Dependencies
- Step 4 (the wrapper code must no longer call the removed functions)

### What NOT to do
- Do not modify `build/mister-wrapper-hps/src/menu.cpp` directly (it is generated)
- Do not remove the MENU_SYSTEM1 hunk from the patch -- it is required for
  system page access (see P-1.3 analysis above)
- Do not remove the patch file entirely -- it still contains the MENU_SYSTEM1
  hunk

### What to do if it fails
- If `git apply` fails: the upstream menu.cpp may have changed at the pinned
  commit. Regenerate the patch by diffing the upstream file against the desired
  output.
- If the build fails with undefined `threesx_wrapper_menu_key_take`: something
  still references it -- grep the overlay files

---

## Step 6: Update threesx_support_stubs.cpp (low priority)

### Title
Remove `threesx_wrapper_menu_key_take()`, `threesx_wrapper_menu_set_visible()`,
`menu_present()`, and `MenuHide()` stubs. Replace with standard-compatible stubs.

### Why it matters
`vendor/Main_MiSTer/threesx_support_stubs.cpp` provides stub implementations
for functions that the full HPS build would normally get from `menu.cpp`. With
the custom menu removed, the stubs for `threesx_wrapper_menu_key_take` and
`threesx_wrapper_menu_set_visible` are no longer needed. However, since these
stubs are NOT in the overlay file list (`main-mister-overlay.files` does not
include `threesx_support_stubs.cpp`), they are only used for standalone/test
builds, not the HPS build.

**Verify:** `threesx_support_stubs.cpp` is NOT listed in
`tools/mister-wrapper/main-mister-overlay.files`. Confirmed -- it is not.
This file is used only for the slim/probe build path.

**P-2.5 note:** This step is low priority because `threesx_support_stubs.cpp`
is dead code in the main build path. It only matters if the slim/probe build
is actively used. Consider merging this cleanup into Step 4 to avoid a separate
step, or defer it entirely.

### Files to read before implementing
- `/Users/sb/Developer/3sx-mister/vendor/Main_MiSTer/threesx_support_stubs.cpp`
- `/Users/sb/Developer/3sx-mister/tools/mister-wrapper/main-mister-overlay.files`

### Files to modify
**`vendor/Main_MiSTer/threesx_support_stubs.cpp`**:

1. Remove `threesx_wrapper_menu_key_take()` (lines 25-30)
2. Remove `threesx_wrapper_menu_set_visible()` (lines 32-35)
3. Update `menu_present()` (lines 37-40) to not reference `g_menu_visible`
   (just return 0, since there is no custom menu)
4. Update `MenuHide()` (lines 42-45) to be a no-op (remove the call to
   `threesx_wrapper_menu_set_visible`)
5. Remove the `g_menu_key` and `g_menu_visible` variables (lines 5-6) if no
   longer used
6. Update `menu_key_set()` -- this is still needed for the slim build path if
   input.cpp calls it, but can be simplified to a no-op or kept as-is

### Success criteria
1. `grep -c 'threesx_wrapper_menu_key_take' vendor/Main_MiSTer/threesx_support_stubs.cpp` returns 0
2. `grep -c 'threesx_wrapper_menu_set_visible' vendor/Main_MiSTer/threesx_support_stubs.cpp` returns 0
3. Slim/probe build still compiles (if applicable)

### Dependencies
- Step 4 (header changes removing the declarations)

### What NOT to do
- Do not remove `menu_key_set()` -- it may still be called by `input.cpp`
- Do not remove `MenuHide()` -- it may still be referenced

### What to do if it fails
- Link errors in slim build: check what still references the removed stubs

---

## Step 7: Integration Test and Cleanup

### Title
Full integration test on MiSTer hardware. Verify all menu options work correctly.

### Why it matters
This is the final validation step. All code changes are complete; this step
verifies the end-to-end behavior.

### Files to read before implementing
- `/Users/sb/Developer/3sx-mister/tools/mister-wrapper/build-release.sh` (if it
  exists, for the deployment workflow)
- `/Users/sb/Developer/3sx-mister/tools/mister/misterctl.sh` (for deployment)

### Test procedure

1. **Build both artifacts:**
   - FPGA: `tools/mister-wrapper/build-core.sh` (use `--fast`, run in colima VM
     with nohup)
   - HPS: `tools/mister-wrapper/build-hps.sh`

2. **Deploy to MiSTer** (never use `rsync --delete`):
   - Copy `3SX.rbf` to `/media/fat/` on MiSTer
   - Copy `MiSTer_3SX` to `/media/fat/` on MiSTer

3. **Test menu navigation:**
   - Press F12: standard MiSTer menu appears with center page showing 3SX options
   - Left page: volume, INI selection (standard)
   - Right page: "Define 3SX buttons", video processing, save settings (standard)
   - Press ESC/F12: menu closes, game resumes

4. **Test each option:**
   - [ ] Show FPS: cycle through Off/FPS/Debug. Verify FPS overlay appears for
         FPS mode, debug overlay for Debug mode, and gone for Off.
   - [ ] Scale Mode: change to Nearest, restart game, verify scale mode applied.
   - [ ] SA Activation: toggle, verify effect quality changes. Specifically verify
         that "Full" in the menu produces Full quality (not Cached BG) -- this
         validates the P-1.2 fix for enum alignment.
   - [ ] SA Ghost Res: toggle, verify ghost resolution changes.
   - [ ] SA Ghost Count: change through 0-4, verify ghost count changes.
   - [ ] Overclock: change to 1000MHz, restart, verify clock speed applied after
         restart (not immediately).
   - [ ] Reset to Default: select "Yes", verify all options reset to defaults
         (FPS=Off, SA Activation=Cached BG, Ghost Res=Full, Ghost Count=4,
         Overclock=Stock). Verify the O-type toggle clears back to "No".
   - [ ] Restart Game: select "Yes", verify game restarts and the toggle clears
         back to "No" after restart.
   - [ ] Reboot (system page): press, verify return to MiSTer menu.

5. **Test button mapping:**
   - Navigate to right page -> "Define 3SX buttons"
   - Complete the mapping wizard
   - Verify buttons work correctly in-game

6. **Test settings persistence:**
   - Change some settings, quit, reload core
   - Verify settings are restored (both in menu display and in game behavior)

7. **Test system page access (P-1.3 / P-2.6 validation):**
   - Navigate to the system settings page (left page -> System Settings)
   - Verify the page is accessible and not skipped
   - If it is skipped: the MENU_SYSTEM1 patch hunk was lost or reverted

8. **Test input passthrough (P-2.10 validation):**
   - Verify that `input_set_joy_passthrough(1)` still works correctly with
     the standard `menu_present()` (no `wrapper_menu_visible`). Specifically:
   - Open the menu, verify joystick input goes to menu navigation
   - Close the menu, verify joystick input goes to the game
   - This tests that removing `wrapper_menu_visible` from `menu_present()` does
     not break the input routing that depends on `menu_present()` returning the
     correct value

9. **Test edge cases:**
   - Open menu during gameplay: game should pause/continue rendering
   - Native video mode: verify menu still works (OSD renders over native output)
   - Forced/probe mode: verify wrapper doesn't crash
   - **Ordering note (P-2.9):** The poller runs before `HandleUI()` in the main
     loop. This means a status change made by HandleUI in iteration N is not
     seen by the poller until iteration N+1 (one frame delay). This is harmless
     for all current settings but should be documented.

### Success criteria
All test items pass. No regressions in gameplay, input, or video.

### Dependencies
- Steps 1-6 all complete

### What NOT to do
- Do not deploy with `rsync --delete` (destroys game data)
- Do not skip the Quartus rebuild (CONF_STR changes require a new `.rbf`)

### What to do if it fails
- **Menu doesn't appear:** Check that the new `.rbf` is loaded (not a stale one).
  Verify CONF_STR is correct in the built `3SX.sv`.
- **Settings don't apply:** Add debug logging in `poll_status_changes()` to print
  status bit values. Check that `user_io_status_get` returns expected values.
- **Button mapping broken:** Verify JOYDIGMAP4 exits to MENU_COMMON1, not
  MENU_NONE1. Check that `joy_bnames` is populated from the J1 CONF_STR line.
- **Settings don't persist across reboot:** Verify Step 3 seeding runs on launch.
  Check that `user_io_status_set` is called after FPGA init.

---

## Summary of Changes by File

| File | Action | Lines Changed (approx) |
|------|--------|----------------------|
| `vendor/Menu_MiSTer/menu.sv` | Add O/R entries to CONF_STR | +12 |
| `vendor/Main_MiSTer/threesx_wrapper.cpp` | Add poller (~60 lines), add seeding (~10 lines), remove custom menu (~270 lines of functions + ~30 lines of globals/enums) | +70, -300 |
| `vendor/Main_MiSTer/threesx_wrapper.h` | Remove 2 function declarations | -2 |
| `vendor/Main_MiSTer/threesx_support_stubs.cpp` | Remove/simplify custom menu stubs | -15 |
| `tools/mister-wrapper/main-mister-full-menu.patch` | Reduce to single MENU_SYSTEM1 hunk | -70 |

**Net: ~+80 / -390 lines. Remove ~700 lines of custom menu code, add ~80 lines
of status polling (with delta-cycle logic).**

---

## Risk Assessment

| Risk | Mitigation |
|------|-----------|
| Signal cycling semantics mismatch | Poller computes modular delta between old and new enum values; sends exact number of cycle signals. Verify game-side signal handlers accept rapid consecutive signals. |
| Status bits silently ignored by old `.rbf` | Always deploy new `.rbf` alongside new HPS binary |
| `menu_present()` change breaks game input passthrough | Test that `input_set_joy_passthrough(1)` still works without `wrapper_menu_visible` (Step 7, test 8) |
| Button mapping wizard exits to wrong state | Test JOYDIGMAP4 thoroughly; easy to fix in patch if wrong |
| Persisted MiSTer settings (`3SX.CFG`) conflict with game config file | Game config is authoritative. Step 3 seeding overwrites `cur_status` bits 10-22 after `user_io_init` loads `.cfg`. |
| MENU_SYSTEM1 hunk lost during patch reduction | Step 5 success criterion 5 explicitly verifies the `is_menu()` guard is present. Patch file must not be emptied. |
| O-type toggle for Reset/Restart shows "Yes" until cleared | Poller clears the bit immediately on detection. If poller is slow, user may see "Yes" for one frame. Harmless. |
