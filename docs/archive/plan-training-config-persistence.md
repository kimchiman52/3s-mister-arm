# Training Config Persistence -- Implementation Plan

## Background

Training mode settings live in `Training[3]` (type `TrainingData`, defined in `structs.h`).
Each element is a struct containing `s8 contents[2][2][6]`.

- **`Training[0]`** -- Active settings used during gameplay.
- **`Training[1]`** -- Snapshot taken when recording starts (parry/dummy recording).
- **`Training[2]`** -- Working copy edited by the menu UI. Copied into `Training[0]` on resume/start.

The array indices of `contents[id][type][slot]` map to:

| id | type | slot | Meaning | Menu group | Max value |
|----|------|------|---------|------------|-----------|
| 0  | 0    | 0    | Dummy Action (Stand/Crouch/Jump/CPU/Human) | Dummy Setting | 4 |
| 0  | 0    | 1    | Guard (Auto/No/All/Parrying/Random/...) | Dummy Setting | 6 |
| 0  | 0    | 2    | Quick Standing (Off/On/Random) | Dummy Setting | 2 |
| 0  | 0    | 3    | Stun (Off/1-Hit/No Gain) | Dummy Setting | 2 |
| 0  | 1    | 0    | S.A. Gauge (Normal/Max Start/Infinity/Maximum) | Training Option | 3 |
| 0  | 1    | 1    | Attack Data (Off/On) | Training Option | 1 |
| 0  | 1    | 2    | Damage Level (1-4 stars) | Training Option | 3 |
| 0  | 1    | 3    | Difficulty (1-8 stars) | Training Option | 7 |
| 0  | 1    | 4    | Hitboxes (Off/On) | Training Option | 1 |
| 0  | 1    | 5    | Input History (Off/On) | Training Option | 1 |
| 1  | 0    | 0    | Parry: Action/Player (Stand/Crouch/Jump) | Blocking Tr Option | 2 |
| 1  | 0    | 1    | Parry: S.A. Gauge/Dummy (Normal/Max Start/Infinity/Maximum) | Blocking Tr Option | 3 |
| 1  | 0    | 2    | Parry: Auto Parrying (Off/On) | Blocking Tr Option | 1 |
| 1  | 0    | 3    | Parry: S.A. Gauge/Player (Normal/Max Start/Infinity/Maximum) | Blocking Tr Option | 3 |
| 1  | 1    | *    | (unused by menu, max values all 0) | -- | -- |

Two standalone globals control display overlays:

- `Disp_Attack_Data` (u8) -- whether attack data overlay is shown
- `Disp_Input_History` (u8) -- whether input history overlay is shown

These are zeroed on entering the training menu and re-derived from `Training->contents[0][1][1]` and `Training[0].contents[0][1][5]` when gameplay resumes (menu.c lines ~4076-4077, ~4144-4145). Since persisting `contents` already captures these values, they do NOT need to be stored in the config file separately.

### Training data lifecycle

1. **Entry from mode select**: `mpp_w.initTrainingData = true`, then `Default_Training_Data(0)` is called from `sel_pl.c:214`. This zeros `Training[0].contents`, sets damage/difficulty from `save_w`, copies to `Training[2]`, and clears the display flags.

2. **Entry from "DEFAULT SETTING" in menus**: `Default_Training_Data(1)` or `Default_Training_Option()` -- always resets regardless of flag. Additionally, Normal Training > Dummy Setting > DEFAULT SETTING (menu.c:5037-5043) directly zeroes `Training[2].contents[0][0][0..3]` without calling `Default_Training_Data`.

3. **Menu editing**: The menu writes directly into `Training[2].contents` via `Dummy_Move_Sub_LR`.

4. **Resume/Start gameplay**: `Training[0] = Training[2]` (via `Setup_NTr_Data` or direct assignment).

### Exit paths from training mode

All exit paths go through `Yes_No_Cursor_Exit_Training`, which presents a Yes/No dialog. Selecting "Yes" calls `Soft_Reset_Sub()` (in `sys_sub.c:1000`), which fades out, stops sound, and re-launches the game task back to the title/mode-select screen. The two call sites:

1. **Normal Training exit** (`menu.c:4830`): `Yes_No_Cursor_Exit_Training(task_ptr, 8)` -- triggered from menu item 8 ("EXIT") in Normal Training.
2. **Parry Training exit** (`menu.c:5344`): `Yes_No_Cursor_Exit_Training(task_ptr, 5)` -- triggered from menu item 5 ("EXIT") in Parry Training.

Both ultimately call `Soft_Reset_Sub()` when the user confirms.

Additionally, **Character Change** (`menu.c:5462`) exits training but goes to character select, then re-enters training. `Default_Training_Data(0)` is called again from `sel_pl.c` on re-entry, which would wipe settings -- this is the key place where loading saved config matters.

### Existing config system (reference only)

- Path resolution: `Paths_GetPrefPath()` returns `SDL_GetPrefPath("CrowdedStreet", "3SX")` (platform-specific app data dir).
- Config file: `{pref_path}/config` -- text-based key-value format.
- The training config will be a separate binary file at `{pref_path}/training` and will NOT use the text config system.

### Build system

`CMakeLists.txt` uses `file(GLOB_RECURSE GAME_SRC src/*.c)`, so any new `.c` file under `src/` is automatically compiled. No CMake changes are needed.

---

## Step 1: Define the training config file format and API **DONE**

### Why it matters
Establishes the on-disk format and the C interface that all other steps depend on.

### Files to read before implementing
- `/Users/sb/Developer/3sx/include/structs.h` (lines 1882-1884, TrainingData definition)
- `/Users/sb/Developer/3sx/src/port/config/config.h` (API style reference)
- `/Users/sb/Developer/3sx/src/port/paths.h` (Paths_GetPrefPath declaration)

### Files to create
- **`/Users/sb/Developer/3sx/src/port/config/training_config.h`**
  ```c
  #ifndef TRAINING_CONFIG_H
  #define TRAINING_CONFIG_H

  /// Load training settings from disk into Training[0] and Training[2].
  /// If file is missing or corrupt, does nothing (caller should init defaults first).
  /// Returns true if settings were loaded successfully.
  bool TrainingConfig_Load(void);

  /// Save current training settings (Training[2]) to disk.
  void TrainingConfig_Save(void);

  #endif
  ```

- **`/Users/sb/Developer/3sx/src/port/config/training_config.c`**
  - Include paths (following the conventions used by other files in `src/port/`):
    ```c
    #include "port/config/training_config.h"
    #include "port/paths.h"
    #include "structs.h"
    #include "sf33rd/Source/Game/system/work_sys.h"

    #include <stdbool.h>
    #include <stdio.h>
    #include <string.h>
    ```
  - Define a file header struct:
    ```c
    #define TRAINING_CONFIG_MAGIC 0x54524E31  // "TRN1"
    #define TRAINING_CONFIG_VERSION 1

    typedef struct {
        u32 magic;
        u32 version;
        s8 contents[2][2][6];  // copy of TrainingData.contents
    } TrainingConfigFile;
    ```
    This struct is 32 bytes (4 + 4 + 24), naturally aligned with no padding. No packing attribute is needed.
  - Define a local max-value table for bounds checking:
    ```c
    static const s8 max_values[2][2][6] = {
        { { 4, 6, 2, 2, 0, 0 }, { 3, 1, 3, 7, 1, 1 } },
        { { 2, 3, 1, 3, 0, 0 }, { 0, 0, 0, 0, 0, 0 } }
    };
    ```
    These values correspond to the first 6 columns of `Menu_Max_Data_Tr[2][2][8]` in menu.c (the max array has 8 columns but contents only uses 6 slots).
  - `TrainingConfig_Load()`:
    1. Call `Paths_GetPrefPath()`. If NULL, return false.
    2. Build path: `"{pref_path}training"`.
    3. Open with `fopen(path, "rb")`. If NULL, return false.
    4. Read `sizeof(TrainingConfigFile)` bytes. If short read, close and return false.
    5. Validate magic == `TRAINING_CONFIG_MAGIC` and version == `TRAINING_CONFIG_VERSION`. If mismatch, close and return false.
    6. Validate all values: iterate over `contents[id][type][slot]` (id=0..1, type=0..1, slot=0..5), clamping any value that exceeds `max_values[id][type][slot]` to 0.
    7. Copy `file.contents` into `Training[0].contents` (via memcpy).
    8. Copy into `Training[2].contents` as well (Training[2] is the working/menu copy).
    9. Close file and return true.
  - `TrainingConfig_Save()`:
    1. Call `Paths_GetPrefPath()`. If NULL, return.
    2. Build path: `"{pref_path}training"`.
    3. Open with `fopen(path, "wb")`. If NULL, return.
    4. Fill `TrainingConfigFile` from `Training[2].contents`.
    5. Write, close.

### Success criteria
- Files compile with no errors or warnings.
- Both functions are callable but not yet wired in. The game builds and runs identically to before.

### Dependencies
None (first step).

### What NOT to do
- Do not modify any existing files in this step.
- Do not use SDL_IOStream or the text-based config helpers -- use plain `fopen`/`fread`/`fwrite` for simplicity and consistency with the binary format.
- Do not add versioned migration logic. Version 1 is the only version. If the version doesn't match, discard and use defaults.

### What to do if it fails
- Compilation errors: Check includes. The file needs `structs.h` for `TrainingData` and `u32`/`s8` types, `port/paths.h` for `Paths_GetPrefPath`, and `sf33rd/Source/Game/system/work_sys.h` for the `Training[3]` extern.
- `sizeof(TrainingConfigFile)` should be exactly 32 bytes (4+4+24). Verify with a `_Static_assert` if in doubt.

---

## Step 2: Load saved config on training mode entry **DONE**

### Why it matters
This is the core "persistence" -- when the player enters training mode, previously saved settings are restored instead of being wiped to defaults.

### Files to read before implementing
- `/Users/sb/Developer/3sx/src/sf33rd/Source/Game/menu/menu.c` (lines 5510-5538, `Default_Training_Data`)
- `/Users/sb/Developer/3sx/src/sf33rd/Source/Game/screen/sel_pl.c` (line 186, the `Default_Training_Data(0)` call site)

### Files to modify
- **`/Users/sb/Developer/3sx/src/sf33rd/Source/Game/menu/menu.c`**
  1. Add `#include "port/config/training_config.h"` near the top includes.
  2. In `Default_Training_Data()`, add a config load call at the end, guarded by `flag == 0`. The load only runs on initial entry (not on "DEFAULT SETTING" resets, which use `flag == 1`). The load overwrites the just-zeroed defaults with saved values; if no file exists or it's corrupt, `TrainingConfig_Load` returns false and defaults remain. After loading, re-apply damage/difficulty to `save_w` since the load may have changed those values.

  Final placement in `Default_Training_Data`:
  ```c
  void Default_Training_Data(s32 flag) {
      s16 ix, ix2, ix3;

      if (flag == 0) {
          if (!mpp_w.initTrainingData) return;
          mpp_w.initTrainingData = false;
      }

      // ... existing zeroing and initialization ...

      Training[2] = Training[0];
      Disp_Attack_Data = 0;
      Disp_Input_History = 0;

      // NEW: Restore persisted settings on initial entry only
      if (flag == 0) {
          if (TrainingConfig_Load()) {
              save_w[Present_Mode].Damage_Level = Training[0].contents[0][1][2];
              save_w[Present_Mode].Difficulty = Training[0].contents[0][1][3];
          }
      }
  }
  ```

### Success criteria
- Build succeeds.
- On first launch (no saved file), behavior is identical to before -- all settings start at 0/defaults.
- On subsequent launches (after Step 3 adds saving), settings persist.

### Dependencies
Step 1 (training_config.h/c must exist).

### What NOT to do
- Do not load in `Default_Training_Option()`. That function is the "DEFAULT SETTING" button which intentionally resets options. Loading saved config there would defeat its purpose.
- Do not load when `flag == 1`. That path is called from the parry training's "DEFAULT SETTING" button (menu.c line 5404) and is an intentional reset. Only load when `flag == 0` (the initial entry path, called from `sel_pl.c`).

### What to do if it fails
- If settings aren't loading, add `printf` in `TrainingConfig_Load` to check if the file is found and if magic/version match.
- If damage/difficulty feel wrong in-game after loading, verify that `save_w[Present_Mode]` is being updated after the load.

---

## Step 3: Save config on training mode exit **DONE**

### Why it matters
Without saving, there is nothing to load. This step wires up the save call at every path that leaves training mode.

### Files to read before implementing
- `/Users/sb/Developer/3sx/src/sf33rd/Source/Game/menu/menu.c` -- all exit paths identified in Background section above.
- `/Users/sb/Developer/3sx/src/sf33rd/Source/Game/system/sys_sub.c` (lines 1000-1015, `Soft_Reset_Sub`)

### Files to modify
- **`/Users/sb/Developer/3sx/src/sf33rd/Source/Game/system/sys_sub.c`**
  1. Add `#include "port/config/training_config.h"` near the top includes.
  2. In `Soft_Reset_Sub()`, add a save call inside the existing training-mode guard:
     ```c
     if (Is_Training_Mode(Mode_Type)) {
         TrainingConfig_Save();           // NEW
         Set_Training_Hitbox_Display(false);
     }
     ```
     This covers both Normal Training and Parry Training exits via the EXIT menu item.

  **Why `Soft_Reset_Sub` and not `Yes_No_Cursor_Exit_Training`?** Because `Soft_Reset_Sub` is the single choke point that all "leave training, go to title" paths funnel through. `Yes_No_Cursor_Exit_Training` just handles the cursor UI; the actual exit happens when it calls `Soft_Reset_Sub`. Placing the save in `Soft_Reset_Sub` is simpler and catches any future exit paths that also use it.

- **`/Users/sb/Developer/3sx/src/sf33rd/Source/Game/menu/menu.c`**
  1. Also save when the user selects **Character Change**, since that exits training gameplay (though it re-enters training with the same settings after character select). Add a `TrainingConfig_Save()` call at the top of `Character_Change()` (line 5462, before `Training_Menu_From_Pause = TRAINING_MENU_DIRECT`):
     ```c
     void Character_Change(struct _TASK* task_ptr) {
         s16 ix;

         TrainingConfig_Save();  // NEW: persist settings before character re-select
         Training_Menu_From_Pause = TRAINING_MENU_DIRECT;
         // ... rest unchanged
     ```

### Success criteria
- Build succeeds.
- Enter training mode, change a setting (e.g., Dummy Action to CROUCH), then exit training via EXIT > Yes.
- Verify that the file `{pref_path}/training` exists and is `sizeof(TrainingConfigFile)` bytes.
- Re-enter training mode. Dummy Action should be CROUCH, not STAND.
- Repeat test with Character Change: change a setting, change character, confirm setting persists.

### Dependencies
Steps 1 and 2.

### What NOT to do
- Do not save on every menu cursor movement or every frame. Save only on exit.
- Do not save on "Resume" (returning to gameplay from the pause menu). The user hasn't left training yet; saving should happen when they actually leave. This also avoids any transient state issues.
- Do not add save calls in `Back_to_Mode_Select` -- that function is used for replay mode exit, not training exit. Training exit goes through `Soft_Reset_Sub`.

### What to do if it fails
- If the file isn't being created, check that `Paths_GetPrefPath()` returns a valid writable directory. Add `printf` logging in `TrainingConfig_Save`.
- If settings don't persist across launches, verify the load path (Step 2) is executing by checking `mpp_w.initTrainingData` is true on entry.
- If settings persist within a session but not across app restarts, verify the file is actually being flushed/closed (check `fclose` return value).

---

## Step 4: Validate edge cases and clean up **DONE**

### Why it matters
Ensures robustness for real-world scenarios: corrupt files, version upgrades, first-time users.

### Files to read before implementing
- The newly created `training_config.c`
- `Default_Training_Data` and `Default_Training_Option` in menu.c

### Verification checklist (no code changes expected)

1. **Missing file**: Delete `{pref_path}/training`. Launch game, enter training. All settings should be defaults. Change a setting, exit. File should be created. Re-enter, setting should persist.

2. **Corrupt file**: Write random bytes to `{pref_path}/training`. Launch game, enter training. Should fall back to defaults gracefully (no crash). Exit training. File should be overwritten with valid data.

3. **Truncated file**: Truncate the file to 4 bytes. Same behavior as corrupt.

4. **Wrong version**: Hex-edit the version field to 99. Same behavior as corrupt.

5. **"DEFAULT SETTING" button**: Enter training with persisted settings. Go to Training Option > DEFAULT SETTING. Settings should reset to defaults. Exit training. Re-enter. Settings should now be the defaults (because the save on exit captured the reset state).

6. **Character Change**: Enter training, change Dummy Action to JUMP. Select Character Change. Pick same characters. Training menu should show Dummy Action = JUMP (loaded from persisted file).

7. **Parry training settings**: Enter parry training, change Auto Parrying to ON. Exit. Re-enter parry training. Auto Parrying should be ON.

8. **Display settings via contents**: Enable Attack Data and Input History in Normal Training (these are `contents[0][1][1]` and `contents[0][1][5]`). Exit training. Re-enter Normal Training. Both should be enabled (the display flags are re-derived from contents on gameplay resume).

**Note**: Attack Data, Hitboxes, and Input History display overlays are only active in Normal Training (`Present_Mode==4`). In Parry Training mode, these display flags are forced to 0 regardless of contents values (see menu.c lines ~4078-4080).

### Files to modify (if needed)
- `training_config.c` -- only if edge case testing reveals issues.

### Success criteria
All 8 verification scenarios pass.

### Dependencies
Steps 1-3.

### What NOT to do
- Do not add migration logic for future versions. When a version 2 is needed, the version 2 implementation should handle it then.
- Do not add error reporting UI. A `printf`/`SDL_Log` on load failure is fine for debugging but no user-facing dialog is needed.

### What to do if it fails
- Out-of-range values: bounds checking is already implemented in `TrainingConfig_Load` (Step 1) using the local `max_values` table, which mirrors the first 6 columns of `Menu_Max_Data_Tr[2][2][8]`. Values exceeding the max are clamped to 0.
- Settings partially applying: double-check that `Training[2] = Training[0]` still executes after the load, so both the active and working copies are in sync.

---

## Summary of all file changes

| File | Action | Description |
|------|--------|-------------|
| `src/port/config/training_config.h` | **Create** | Header with `TrainingConfig_Load` and `TrainingConfig_Save` declarations |
| `src/port/config/training_config.c` | **Create** | Binary file I/O implementation (~80 lines) |
| `src/sf33rd/Source/Game/menu/menu.c` | **Modify** | Add include, add `TrainingConfig_Load()` call in `Default_Training_Data`, add `TrainingConfig_Save()` in `Character_Change` |
| `src/sf33rd/Source/Game/system/sys_sub.c` | **Modify** | Add include, add `TrainingConfig_Save()` call in `Soft_Reset_Sub` |

No changes to `CMakeLists.txt` (GLOB_RECURSE picks up new files automatically).
No changes to `structs.h`, `work_sys.h`, or `workuser.h`.
