#ifndef PORT_CONFIG_BGM_TYPE_H
#define PORT_CONFIG_BGM_TYPE_H

#include "structs.h"

/// Apply the `bgm-type` config key (if explicitly present on disk) over the
/// BgmType just deserialized from the settings save file. Must be called
/// AFTER Init_Task_Aload's settings load completes -- savesub.c's
/// deserialize_settings() sets `sys_w.bgm_type = save_w[1].BgmType` as part
/// of that load (src/sf33rd/Source/PS2/mc/savesub.c) -- see the call site in
/// src/sf33rd/Source/Game/init3rd.c. This lets the MiSTer OSD's BGM Type
/// toggle (status bit [14]) win at boot, the same config-overrides-save
/// ordering Arcade Balance uses for status bit [30]
/// (thirdsarm_wrapper.cpp's seed-from-game-config block).
void BgmType_ApplyBootOverride(void);

/// Two-way sync: call after the in-game Sound Options menu changes
/// sys_w.bgm_type (menu.c's Sound_Test) so the on-disk `bgm-type` key --
/// and therefore what the OSD seeds from at the next launch -- reflects the
/// in-game choice. Best-effort / fire-and-forget, matching the wrapper's own
/// write_runtime_*_default() helpers.
void BgmType_PersistToConfig(BgmType type);

#endif
