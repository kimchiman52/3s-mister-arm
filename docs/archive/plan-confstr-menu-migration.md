# Plan: Migrate 3SX wrapper menu to CONF_STR-driven MiSTer common menu

**Status: IMPLEMENTED** — See commit `d0c8754b`. Implementation plan:
`docs/plan-confstr-menu-migration-impl.md`.

## Summary

Replace the custom wrapper OSD menu (`draw_wrapper_menu` / `service_wrapper_menu`
in `threesx_wrapper.cpp`) with the standard MiSTer CONF_STR-driven core menu.
This gives the 3SX core the familiar left/center/right page navigation that all
MiSTer cores use, and gets button mapping, video processing, audio filters,
save/load settings, and reboot for free.

---

## Current architecture

The wrapper (`threesx_wrapper.cpp`) has a fully custom OSD menu:

- `draw_wrapper_menu()` (~50 lines) renders items via `OsdWrite()` calls
- `service_wrapper_menu()` (~200 lines) handles key input and menu actions
- `format_wrapper_value_line()` formats label/value pairs
- Menu items: Resume, Define Buttons, FPS, Scale Mode, SA Activation,
  SA Ghost Res, SA Ghost Count, Overclock, Reset to Default, Restart, Quit
- Actions are applied via `kill(child, signal)` or config file writes
- The wrapper intercepts all OSD keys (F12/Menu) before `HandleUI()` sees them
- A "passthrough" mode was added to let `HandleUI()` drive the button mapping
  wizard, requiring `mgl_get()->done = 1`, `OsdUpdate()` after `HandleUI()`,
  `wrapper_menu_visible` cleanup, and `input_is_mapping_active()` polling

This is ~700+ lines of custom menu code and fragile `HandleUI()` integration.

---

## Proposed architecture

### CONF_STR options

All wrapper settings become `O` (option), `T` (trigger), or `R` (reset-trigger)
entries in CONF_STR. The MiSTer framework renders them in the standard core menu
(center page) and writes to the `status` register when the user changes values.

```verilog
localparam CONF_STR = {
    "MENU;UART31250,MIDI;",
    "J1,LP,MP,HP,LK,MK,HK,Select,Start;",
    "jn,Y,X,L,B,A,R,Select,Start;",
    "-;",
    "O[10:10],Show FPS,Off,On;",
    "O[11:12],Scale Mode,Auto,Native,Nearest;",
    "-;",
    "O[13:13],SA Activation,Cached BG,Full;",
    "O[14:14],SA Ghost Res,Full,Half;",
    "O[15:17],SA Ghost Count,0,1,2,3,4;",
    "-;",
    "O[18:19],Overclock,Stock,1000MHz,1200MHz;",
    "-;",
    "T[20],Reset to Default;",
    "R[21],Restart Game;",
    "R[22],Quit to MiSTer;",
    "-;",
    "V,v",`BUILD_DATE
};
```

Status bits 0-9 are reserved by the core (bit 0 = reset, bit 4 = PAL, bit 5 = FB,
bits 6-8 = LED, bit 9 = NATIVE_VID). Bits 10+ are safe for menu options.

The `"MENU;UART31250,MIDI;"` token gets replaced with `"3SX;;"` by the build
script's Ruby patcher. Only the lines after it matter for menu options.

### Status bit layout

| Bits   | CONF_STR     | Values                          | Signal/Action                       |
|--------|--------------|---------------------------------|-------------------------------------|
| [10]   | Show FPS     | 0=Off, 1=On                     | `kill(child, kRuntimeFpsToggleSignal)` |
| [12:11]| Scale Mode   | 0=Auto, 1=Native, 2=Nearest     | `write_runtime_scale_mode_default()` |
| [13]   | SA Activation| 0=Cached BG, 1=Full             | `kill(child, kRuntimeSuperEffectQualityCycleSignal)` |
| [14]   | SA Ghost Res | 0=Full, 1=Half                  | `kill(child, kRuntimeGhostResolutionCycleSignal)` |
| [17:15]| SA Ghost Count| 0-4                            | `kill(child, kRuntimeGhostCountCycleSignal)` |
| [19:18]| Overclock    | 0=Stock, 1=1000MHz, 2=1200MHz   | `kill(child, kRuntimeArmClockCycleSignal)` |
| [20]   | Reset Default| Trigger pulse                   | Zero bits [10:19], re-apply         |
| [21]   | Restart Game | Trigger pulse + close OSD       | `kill(child, SIGTERM)` + restart    |
| [22]   | Quit to MiSTer| Trigger pulse + close OSD      | `kill(child, SIGTERM)` + quit       |

### ARM-side status polling

The wrapper's main loop (`wait_for_child`) already runs at ~1kHz. Replace the
custom menu handling with a status-change poller:

```cpp
// Snapshot before HandleUI
uint32_t prev_status_hi = user_io_status_get("[19:10]");

HandleUI();
OsdUpdate();

// Compare after HandleUI
uint32_t new_status_hi = user_io_status_get("[19:10]");
if (new_status_hi != prev_status_hi) {
    apply_status_changes(child, prev_status_hi, new_status_hi);
}

// Check trigger bits (pulses that auto-clear)
if (user_io_status_get("[20]")) {
    // Reset to Default: zero bits 10-19, preserve 0-9
    user_io_status_set("[19:10]", 0);
    apply_all_defaults(child);
}
if (user_io_status_get("[21]")) {
    // Restart
    g_wrapper_restart_requested = 1;
    kill(child, SIGTERM);
}
if (user_io_status_get("[22]")) {
    // Quit
    kill(child, SIGTERM);
}
```

`user_io_status_get()` reads from ARM memory (the `cur_status` array). Zero
overhead, no SPI. The status is the authoritative copy — it gets pushed to
FPGA via SPI when modified, but the FPGA ignores bits 10+ for 3SX.

### Three-page navigation (comes for free)

- **Left page (MENU_MISC1):** Volume, INI selection, information display
- **Center page (MENU_GENERIC_MAIN1):** Core settings from CONF_STR (FPS, Scale,
  SA options, Overclock, Restart, Quit)
- **Right page (MENU_COMMON1):** System settings including:
  - Core load
  - **Define joystick buttons** (the button mapping wizard — free, no custom code)
  - Button/Key remap for individual controllers
  - Reset player assignment
  - Video processing (scanlines, filters)
  - Audio filter
  - Save settings / Reset settings
  - Help / About
  - Reboot

### Button mapping

With the standard menu, button mapping works natively:

1. User navigates to right page → "Define 3SX buttons"
2. MiSTer framework runs the JOYDIGMAP wizard using `joy_bnames` from J1 CONF_STR
3. Mapping is saved to `config/inputs/3SX_input_<controller>.map`
4. Applied automatically on next core load

No passthrough mode, no `HandleUI()` plumbing, no `mgl_get()->done` hacks.

---

## What changes

### Files modified

| File | Change |
|------|--------|
| `vendor/Menu_MiSTer/menu.sv` | Update CONF_STR with O/T/R options |
| `vendor/Main_MiSTer/threesx_wrapper.cpp` | Remove custom menu (~700 lines), add status poller (~50 lines) |
| `vendor/Main_MiSTer/threesx_wrapper.cpp` | Remove `service_wrapper_menu()`, `draw_wrapper_menu()`, `format_wrapper_value_line()` |
| `vendor/Main_MiSTer/threesx_wrapper.cpp` | Remove passthrough mode, `mgl_get()->done` hack, `MenuHide()` call |
| `vendor/Main_MiSTer/threesx_wrapper.cpp` | Remove `#include "support/arcade/mra_loader.h"` |
| `vendor/Main_MiSTer/threesx_wrapper.cpp` | Remove `WrapperMenuItem` enum, `g_wrapper_menu_visible`, `g_wrapper_menu_selected`, `g_wrapper_menu_passthrough` |
| `vendor/Main_MiSTer/input.h` | Remove `input_is_mapping_active()` (no longer needed) |
| `vendor/Main_MiSTer/input.cpp` | Remove `input_is_mapping_active()` |
| `tools/mister-wrapper/main-mister-full-menu.patch` | Revert the `MENU_SYSTEM1` `is_menu()` change (no longer needed — standard menu handles it) |

### Files NOT modified

| File | Why |
|------|-----|
| `src/port/sdl/sdl_pad.c` | `get_mister_state()` mapping unchanged |
| `src/sf33rd/Source/Game/io/ioconv.c` | Game button conversion unchanged |
| `vendor/Main_MiSTer/mister_joy_shm.h` | Shared memory format unchanged |

---

## What we gain

1. **Standard MiSTer UX.** Users get the familiar three-page menu. No learning curve.
2. **Button mapping for free.** No custom passthrough/HandleUI integration.
3. **Save/Load settings for free.** MiSTer's "Save settings" serializes `cur_status` to
   a config file (`config/3SX.CFG`). Settings persist across reboots.
4. **~700 lines of code removed.** Less code to maintain, fewer bugs.
5. **Video processing, audio filters, volume** — available on the system page.
6. **Per-controller button remap** — available on the system page.

## What we lose (and mitigations)

| Lost feature | Mitigation |
|---|---|
| Explicit "Resume" menu item | F12/ESC closes OSD (standard MiSTer behavior) |
| Custom "Reset to Default" | `T` trigger that zeroes bits 10-19 ARM-side |
| Pixel-perfect OSD layout | Standard CONF_STR formatting (what users expect) |
| Instant setting application via signals | Status polling in main loop (~1ms latency, imperceptible) |

## Reset to Default handling

The built-in "Reset settings" on MENU_COMMON1 zeroes ALL status bits including
bits 0-9 (native video, FB, LED). Two options:

**Option A: T trigger (recommended).** Add `T[20],Reset to Default;` to CONF_STR.
The wrapper detects the pulse and zeroes only bits 10-19 while preserving 0-9.

**Option B: Re-apply protected bits.** Let the built-in reset zero everything,
then immediately re-apply bits 0-9 in the status poller. ~5 lines of code.

---

## Key technical details

### CONF_STR option format

```
O[start:end],Label,Value0,Value1,...;
```

- Bit range `[start:end]` (inclusive), or `[bit]` for single bit
- Values are comma-separated strings shown in the OSD
- User cycles through values with left/right or enter
- Status bits are updated via `user_io_status_set()`
- `user_io_status_get()` reads from ARM memory (no SPI)

### Status register API

```cpp
// Read specific bits (ARM memory, zero overhead)
uint32_t val = user_io_status_get("[12:11]");

// Write specific bits (updates ARM memory + pushes to FPGA via SPI)
user_io_status_set("[12:11]", 2);
```

The bit range string format matches CONF_STR: `"[start:end]"` or `"[bit]"`.

### HandleUI integration

With the standard menu, the wrapper's main loop simplifies to:

```cpp
input_poll(0);
// copy joy_mask to shared memory
HandleUI();
OsdUpdate();
poll_status_changes(child);  // new: ~50 lines
usleep(1000);
```

No `service_wrapper_menu()`, no passthrough mode, no key interception.

### Wrapper no longer intercepts F12/Menu

Currently the wrapper intercepts F12/Menu via `threesx_wrapper_menu_key_take()`
to show its custom menu. With the standard menu, `HandleUI()` handles F12/Menu
natively to show/hide the CONF_STR-driven core menu.

The wrapper should NOT call `threesx_wrapper_menu_key_take()` at all. Keys flow
directly from `input_poll()` → `menu_key_set()` → `HandleUI()`.

---

## Estimated effort

- CONF_STR definition: ~30 minutes
- Status poller implementation: ~1-2 hours
- Remove custom menu code: ~30 minutes
- Testing all settings: ~1-2 hours
- FPGA rebuild (Quartus): ~90 minutes (automated)
- HPS wrapper rebuild: ~5 minutes (automated)

Total: ~4-6 hours including testing.
