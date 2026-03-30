# Plan: Shared-memory joystick input from MiSTer wrapper to 3SX game

## Problem

The MiSTer OSD button mapper has **no effect** on the 3SX game. Users map
Start (and all other buttons) in the OSD, but the game uses SDL3's own gamepad
database via a completely independent code path. The disconnect:

| Step | MiSTer OSD path (unused by game) | Game's SDL path (actual) |
|------|----------------------------------|--------------------------|
| 1 | User maps buttons in OSD | SDL scans `/dev/input/js*` or `/dev/input/event*` |
| 2 | Framework reads evdev, applies mapping | SDL looks up controller GUID in its gamepad DB |
| 3 | Sends mapped `joy_mask` to FPGA via SPI | Maps raw button indices to `SDL_GAMEPAD_BUTTON_*` |
| 4 | FPGA receives it — game never reads it | Game reads `SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START)` |

The Start button fails because `SDL_HINT_JOYSTICK_LINUX_CLASSIC=1` forces SDL
to use the `/dev/input/js*` interface (needed to work around the wrapper's
`EVIOCGRAB` on evdev). The js interface uses different button numbering than
SDL's gamepad database expects, and Start is one of the buttons that doesn't
align.

## Solution: shared-memory joystick state

The wrapper already builds a perfectly-mapped `joy_mask` per player each frame
(`input.cpp:5986-6002`). Write it to shared memory; the game reads it instead
of SDL gamepads.

This makes the MiSTer OSD button mapper **actually work** for 3SX — the same
way it works for every FPGA core.

---

## Shared-memory layout

A single flat structure at a well-known path, memory-mapped by both processes.

```c
// File: include/port/mister/mister_joy_shm.h  (shared between wrapper and game)

#define MISTER_JOY_SHM_PATH "/dev/shm/threesx-joy"
#define MISTER_JOY_SHM_MAGIC 0x334A5358   // "3JSX"
#define MISTER_JOY_SHM_VERSION 1
#define MISTER_JOY_MAX_PLAYERS 2           // 3SX only uses P1 + P2

typedef struct {
    uint32_t magic;                        // MISTER_JOY_SHM_MAGIC
    uint32_t version;                      // MISTER_JOY_SHM_VERSION
    uint32_t joy_mask[MISTER_JOY_MAX_PLAYERS];
    //
    // Analog sticks: signed 8-bit per axis, centered at 0.
    // Range: -128..+127  (same format as user_io_l_analog_joystick).
    // Zero = no stick data available (digital-only controller).
    //
    int8_t   left_stick_x[MISTER_JOY_MAX_PLAYERS];
    int8_t   left_stick_y[MISTER_JOY_MAX_PLAYERS];
    int8_t   right_stick_x[MISTER_JOY_MAX_PLAYERS];
    int8_t   right_stick_y[MISTER_JOY_MAX_PLAYERS];
} MisterJoyShm;
```

Total size: 28 bytes. Fits in a single cache line.

### `joy_mask` bit layout

The wrapper builds `joy_mask` via `1 << SYS_BTN_*` in the standard mapped-
controller path (`input.cpp:3427`). The bit positions follow `SYS_BTN` indices,
**not** the legacy `JOY_*` defines in `user_io.h` (which are for older 4-button
cores and have different positions for Select/Start).

```
Bit 0  (0x001)  SYS_BTN_RIGHT    d-pad right
Bit 1  (0x002)  SYS_BTN_LEFT     d-pad left
Bit 2  (0x004)  SYS_BTN_DOWN     d-pad down
Bit 3  (0x008)  SYS_BTN_UP       d-pad up
Bit 4  (0x010)  SYS_BTN_A        A  (bottom face / Cross / Xbox A)
Bit 5  (0x020)  SYS_BTN_B        B  (right face  / Circle / Xbox B)
Bit 6  (0x040)  SYS_BTN_X        X  (left face   / Square / Xbox X)
Bit 7  (0x080)  SYS_BTN_Y        Y  (top face    / Triangle / Xbox Y)
Bit 8  (0x100)  SYS_BTN_L        L  (left shoulder / L1)
Bit 9  (0x200)  SYS_BTN_R        R  (right shoulder / R1)
Bit 10 (0x400)  SYS_BTN_SELECT   Select / Back
Bit 11 (0x800)  SYS_BTN_START    Start            <-- the broken button
Bits 12-15      (unused by standard mapping path, reserved)
```

> **Warning — `JOY_START` (0x80) != `1 << SYS_BTN_START` (0x800).**
> The `JOY_*` defines in `user_io.h` reflect a legacy 4-button layout
> (BTN1/BTN2/SELECT/START at bits 4-7). The standard mapped-controller
> path uses `1 << SYS_BTN_*` which puts Select at bit 10 and Start at
> bit 11. The shared memory uses the `1 << SYS_BTN_*` positions because
> that is what `build_joy_mask()` returns for properly mapped controllers.

### `joy_mask` to `SDLPad_ButtonState` mapping

```
joy_mask bit  →  SDLPad_ButtonState field    →  downstream PS2 button
──────────────────────────────────────────────────────────────────────
0x001 RIGHT   →  dpad_right                  →  sw0.bits.right
0x002 LEFT    →  dpad_left                   →  sw0.bits.left
0x004 DOWN    →  dpad_down                   →  sw0.bits.down
0x008 UP      →  dpad_up                     →  sw0.bits.up
0x010 A       →  south                       →  sw1.bits.cross
0x020 B       →  east                        →  sw1.bits.circle
0x040 X       →  west                        →  sw1.bits.square
0x080 Y       →  north                       →  sw1.bits.triangle
0x100 L       →  left_shoulder               →  sw1.bits.l1
0x200 R       →  right_shoulder              →  sw1.bits.r1
0x400 SELECT  →  back                        →  sw0.bits.select
0x800 START   →  start                       →  sw0.bits.start
```

For triggers (L2/R2): joy_mask only has digital bits. When set, populate
`left_trigger` / `right_trigger` with `SDL_MAX_SINT16` (32767) to exceed
the `SDLPAD_TRIGGER_PRESS_THRESHOLD` (4096) in `sdk_libpad2.c:8`.

For analog sticks: scale the MiSTer signed 8-bit values (-128..+127) to
SDL's Sint16 range (-32768..+32767) via `value * 256`.

---

## Implementation steps

### Step 1 — Header: shared memory format

Create `include/port/mister/mister_joy_shm.h` with the struct above. This
header is included by both the wrapper and the game. Keep it C-compatible
(no C++ types) since the wrapper is C++ and the game is C.

### Step 2 — Wrapper: write joystick state to shared memory

**File: `vendor/Main_MiSTer/threesx_wrapper.cpp`**

1. In `threesx_wrapper_run`, before the `for(;;)` launch loop (~line 1594),
   create and mmap the shared memory file:

   ```c
   int shm_fd = open(MISTER_JOY_SHM_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
   ftruncate(shm_fd, sizeof(MisterJoyShm));
   MisterJoyShm *shm = mmap(NULL, sizeof(MisterJoyShm), PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_fd, 0);
   close(shm_fd);  // mapping stays valid
   shm->magic = MISTER_JOY_SHM_MAGIC;
   shm->version = MISTER_JOY_SHM_VERSION;
   ```

2. After `input_poll(0)` in `wait_for_child` (~line 1505), copy the current
   joystick state into shared memory. This requires access to the `joy_mask`
   that `input_poll` builds.

   **Problem:** `joy_mask` is a local variable inside `input_poll`. We need to
   expose it. Options:

   - **(a) Add a getter function to `input.cpp`:**
     ```c
     // input.h
     void input_get_joy_mask(uint32_t *out, int count);

     // input.cpp — after line 6004
     static uint32_t joy_mask_snapshot[NUMPLAYERS];
     // copy joy_mask into snapshot at the end of the grabbed block
     // ...
     void input_get_joy_mask(uint32_t *out, int count) {
         for (int i = 0; i < count && i < NUMPLAYERS; i++)
             out[i] = joy_mask_snapshot[i];
     }
     ```

   - **(b) Expose `joy_mask_prev` which is already static and persists between
     frames.** `joy_mask_prev[NUMPLAYERS]` at `input.cpp:5945` holds the last
     sent mask. Add a simple getter:
     ```c
     void input_get_joy_mask(uint32_t *out, int count);
     ```
     This is the simpler option since `joy_mask_prev` already exists and
     tracks the latest state.

   Choose option (b).

3. In `wait_for_child`, after `input_poll(0)`:
   ```c
   if (shm) {
       uint32_t masks[2];
       input_get_joy_mask(masks, 2);
       shm->joy_mask[0] = masks[0];
       shm->joy_mask[1] = masks[1];
       // analog sticks: populate from analog joystick state if available
       // (secondary concern — leave as zero initially)
   }
   ```

4. On cleanup (before returning from `threesx_wrapper_run`):
   ```c
   if (shm) munmap(shm, sizeof(MisterJoyShm));
   unlink(MISTER_JOY_SHM_PATH);
   ```

5. Pass the shm path to the game via environment variable:
   ```c
   setenv("THREESX_JOY_SHM", MISTER_JOY_SHM_PATH, 1);
   ```

### Step 3 — Game: read joystick state from shared memory

**File: `src/port/sdl/sdl_pad.c`**

1. Add a new input source type:
   ```c
   typedef enum SDLPad_InputType {
       SDLPAD_INPUT_NONE = 0,
       SDLPAD_INPUT_GAMEPAD,
       SDLPAD_INPUT_KEYBOARD,
   #if defined(PORT_MISTER)
       SDLPAD_INPUT_MISTER_SHM,
   #endif
   } SDLPad_InputType;
   ```

2. Add MiSTer shared memory state:
   ```c
   #if defined(PORT_MISTER)
   #include "port/mister/mister_joy_shm.h"
   static const MisterJoyShm *mister_shm = NULL;
   #endif
   ```

3. In `SDLPad_Init()`, before the SDL gamepad scan, try to open and mmap
   the shared memory:
   ```c
   #if defined(PORT_MISTER)
   const char *shm_path = SDL_getenv("THREESX_JOY_SHM");
   if (shm_path) {
       int fd = open(shm_path, O_RDONLY);
       if (fd >= 0) {
           mister_shm = mmap(NULL, sizeof(MisterJoyShm), PROT_READ,
                             MAP_SHARED, fd, 0);
           close(fd);
           if (mister_shm != MAP_FAILED
               && mister_shm->magic == MISTER_JOY_SHM_MAGIC
               && mister_shm->version == MISTER_JOY_SHM_VERSION) {
               // Set up both player slots as MiSTer SHM sources
               for (int i = 0; i < INPUT_SOURCES_MAX; i++) {
                   input_sources[i].type = SDLPAD_INPUT_MISTER_SHM;
                   connected_input_sources++;
               }
               SDL_Log("SDLPad: using MiSTer shared-memory joystick input");
               return;  // skip SDL gamepad/keyboard init
           }
           // mmap failed or wrong magic — fall through to SDL path
           if (mister_shm != MAP_FAILED) munmap((void*)mister_shm, sizeof(MisterJoyShm));
           mister_shm = NULL;
       }
   }
   #endif
   // ... existing SDL gamepad init ...
   ```

4. Add `get_mister_state()`:
   ```c
   #if defined(PORT_MISTER)
   static void get_mister_state(int id, SDLPad_ButtonState *state) {
       SDL_zerop(state);
       if (!mister_shm || id < 0 || id >= MISTER_JOY_MAX_PLAYERS) return;

       const uint32_t m = mister_shm->joy_mask[id];

       state->dpad_right      = (m >> 0)  & 1;
       state->dpad_left       = (m >> 1)  & 1;
       state->dpad_down       = (m >> 2)  & 1;
       state->dpad_up         = (m >> 3)  & 1;
       state->south           = (m >> 4)  & 1;   // A  → Cross
       state->east            = (m >> 5)  & 1;   // B  → Circle
       state->west            = (m >> 6)  & 1;   // X  → Square
       state->north           = (m >> 7)  & 1;   // Y  → Triangle
       state->left_shoulder   = (m >> 8)  & 1;   // L  → L1
       state->right_shoulder  = (m >> 9)  & 1;   // R  → R1
       state->back            = (m >> 10) & 1;   // Select
       state->start           = (m >> 11) & 1;   // Start

       // Digital-only triggers: full pressure when bit set
       state->left_trigger    = (m & 0x1000) ? SDL_MAX_SINT16 : 0;
       state->right_trigger   = (m & 0x2000) ? SDL_MAX_SINT16 : 0;
       state->left_stick      = (m >> 14) & 1;   // L3
       state->right_stick     = (m >> 15) & 1;   // R3

       // Analog sticks: scale int8 (-128..+127) → Sint16 (-32768..+32767)
       state->left_stick_x    = (Sint16)mister_shm->left_stick_x[id]  * 256;
       state->left_stick_y    = (Sint16)mister_shm->left_stick_y[id]  * 256;
       state->right_stick_x   = (Sint16)mister_shm->right_stick_x[id] * 256;
       state->right_stick_y   = (Sint16)mister_shm->right_stick_y[id] * 256;
   }
   #endif
   ```

5. Update `SDLPad_GetButtonState()` and `SDLPad_IsGamepadConnected()`:
   ```c
   void SDLPad_GetButtonState(int id, SDLPad_ButtonState *state) {
   #if defined(PORT_MISTER)
       if (mister_shm) { get_mister_state(id, state); return; }
   #endif
       if (id == keyboard_index) { get_keyboard_state(state); }
       else { get_gamepad_state(id, state); }
   }

   bool SDLPad_IsGamepadConnected(int id) {
   #if defined(PORT_MISTER)
       if (mister_shm && id >= 0 && id < MISTER_JOY_MAX_PLAYERS) return true;
   #endif
       return input_sources[id].type != SDLPAD_INPUT_NONE;
   }
   ```

### Step 4 — Wrapper: expose `joy_mask_prev` via getter

**File: `vendor/Main_MiSTer/input.cpp`**

After the `if (grabbed)` block (~line 6004), the `joy_mask_prev` array holds
the latest button state. Add:

```c
static uint32_t joy_mask_export[NUMPLAYERS] = {};

// Inside input_poll, after line 6004 (end of grabbed block):
for (int i = 0; i < NUMPLAYERS; i++)
    joy_mask_export[i] = joy_mask_prev[i];
```

Then at bottom of file:
```c
void input_get_joy_mask(uint32_t *out, int count) {
    for (int i = 0; i < count && i < NUMPLAYERS; i++)
        out[i] = joy_mask_export[i];
}
```

**File: `vendor/Main_MiSTer/input.h`**

Add declaration:
```c
void input_get_joy_mask(uint32_t *out, int count);
```

### Step 5 — Handle `grabbed == 0` case

When `grabbed` is 0, `input_poll` clears `key_states` and sends zeros to the
FPGA (`input.cpp:6007-6013`). This means `joy_mask_prev` is also zeroed. The
shared memory would always read as zero.

**Fix:** The wrapper must ensure `grabbed` stays 1 so `input_poll` continues
building `joy_mask`. This is already the default (`static int grabbed = 1`).
No change needed — just don't call `input_switch(0)` before the game.

This means the evdev devices stay grabbed. The game does NOT use SDL gamepads
on MiSTer when shared memory is active (it skips SDL gamepad init entirely in
step 3). So the grab is irrelevant to the game.

### Step 6 — Remove `SDL_HINT_JOYSTICK_LINUX_CLASSIC` (optional cleanup)

**File: `src/port/sdl/sdl_app.c`**

When shared memory is active, SDL gamepad init is skipped entirely. The
`JOYSTICK_LINUX_CLASSIC` hint becomes irrelevant. It can be left in place as
a harmless no-op, or removed. Leaving it is safer for fallback if shared
memory is unavailable.

### Step 7 — Analog stick data (secondary)

The wrapper sends analog stick data to the FPGA via separate calls:
`user_io_l_analog_joystick(num, x, y)` and `user_io_r_analog_joystick(num, x, y)`.
These use signed 8-bit values.

To populate the shared memory analog fields, we need to intercept or duplicate
these values. Options:

- **(a)** Add a similar getter: `input_get_analog_joystick(int player, int8_t *lx, int8_t *ly, int8_t *rx, int8_t *ry)`
- **(b)** Have `user_io_l_analog_joystick` / `user_io_r_analog_joystick` write to a static buffer that the wrapper reads

Since 3rd Strike is primarily a d-pad game, **analog sticks can be deferred
to a follow-up**. The shared memory fields exist (zeroed) and will be
populated later without any format change.

---

## Files modified

| File | Change |
|------|--------|
| `include/port/mister/mister_joy_shm.h` | **New.** Shared struct definition. |
| `vendor/Main_MiSTer/input.h` | Add `input_get_joy_mask` declaration. |
| `vendor/Main_MiSTer/input.cpp` | Add `joy_mask_export` + getter (~10 lines). |
| `vendor/Main_MiSTer/threesx_wrapper.cpp` | Create shm, copy joy_mask each frame, cleanup (~30 lines). |
| `src/port/sdl/sdl_pad.c` | MiSTer shm init, `get_mister_state`, route in `GetButtonState` (~60 lines). |

No changes to game logic (`ps2PAD.c`, `ioconv.c`, `pause.c`, etc.). The
shared memory feeds into the same `SDLPad_ButtonState` interface that
downstream code already consumes.

---

## Testing

1. **Verify Start button works** — map Start in OSD, press it in-game, confirm pause triggers.
2. **Verify all other buttons** — directions, face buttons, shoulders, triggers, select.
3. **Verify 2-player** — both P1 and P2 should read from `joy_mask[0]` and `joy_mask[1]`.
4. **Verify fallback** — if `THREESX_JOY_SHM` env var is unset or file doesn't exist, game falls back to SDL gamepad path (existing behavior).
5. **Verify no latency regression** — shared memory is a single cache-line read, should be sub-microsecond.
6. **Verify wrapper OSD menu still works** — menu button combo opens OSD, navigation works, settings apply.

---

## Open questions

- **A/B button convention.** The plan maps MiSTer A (bit 4) → SDL South (bottom face). If the 3SX FPGA core or user community expects a different convention (e.g., SNES-style where A = right face), the mapping table in `get_mister_state` needs adjustment. This should be validated with the actual button mapping that the 3SX FPGA core's `CONF_STR` defines.

- **Analog sticks.** Deferred. The shared memory struct reserves fields but the wrapper doesn't populate them in the initial implementation. This only matters for users who use analog stick for movement (rare in fighting games).

- **Autofire.** The wrapper applies autofire masks before writing `joy_mask_prev`. These carry through to shared memory automatically. No special handling needed.

---

## Alternative approaches (not chosen, for reference)

### Option 1: Release input grabs (simplest, ~2 lines)

Call `input_switch(0)` in the wrapper before the fork. Remove
`SDL_HINT_JOYSTICK_LINUX_CLASSIC` from the game. SDL reads evdev directly
with its correct gamepad database.

**Pros:** Minimal code change.
**Cons:**
- MiSTer OSD button mapping still has no effect on the game. Users who
  remap buttons in the OSD would see the remapping ignored by 3SX.
- The game receives input during wrapper OSD navigation (minor).
- Without `EVIOCGRAB`, keyboard events can bleed into the Linux VT.
  `SDL_HINT_MUTE_CONSOLE_KEYBOARD` mitigates this on the game side but
  not on the wrapper side.
- Relies on SDL's gamepad database being correct for the user's controller,
  which may not always be the case for generic/off-brand controllers common
  in the MiSTer community.

**How it would work:**
1. `threesx_wrapper.cpp`: add `input_switch(0);` before the fork (~line 1643)
2. `sdl_app.c`: remove the `SDL_SetHint(SDL_HINT_JOYSTICK_LINUX_CLASSIC, "1");` line
3. Optionally keep `JOYSTICK_LINUX_CLASSIC` behind a fallback env var check

### Option 2: Selective grab — don't grab gamepads, only keyboards

Modify `input.cpp` to detect whether a device is a gamepad (check for
`EV_ABS` capability via `ioctl(fd, EVIOCGBIT(0, ...), ...)` + `test_bit(EV_ABS, ...)`)
and skip `EVIOCGRAB` on gamepads. Keyboards stay grabbed.

**Pros:** More targeted than option 1. Keyboards don't bleed into VT.
**Cons:**
- Same fundamental problem as option 1: MiSTer OSD mapping still ignored.
- Requires modifying vendored `input.cpp` with device-classification logic
  that doesn't exist today (no `is_gamepad` field in `devInput`).
- Would need changes in two places: the device-open loop (~line 5190) and
  `input_switch()` (~line 6082).
- Still relies on SDL's gamepad database being correct.

**How it would work:**
1. `input.cpp` device enumeration (~line 4858): after opening a device, query
   capabilities with `ioctl(fd, EVIOCGBIT(0, sizeof(evtype_b)), evtype_b)`.
   Set `input[n].is_gamepad = test_bit(EV_ABS, evtype_b)`.
2. Line 5190: change to `ioctl(pool[n].fd, EVIOCGRAB, (!input[n].is_gamepad && (grabbed | user_io_osd_is_visible())) ? 1 : 0);`
3. `input_switch()` line 6082: same filter.
4. Game side: same as option 1 — remove `JOYSTICK_LINUX_CLASSIC`.
