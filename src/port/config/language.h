#ifndef PORT_CONFIG_LANGUAGE_H
#define PORT_CONFIG_LANGUAGE_H

#include "structs.h"

/// Apply the `language` config key over the Language just deserialized from
/// the settings save file. Must be called AFTER Init_Task_Aload's settings
/// load completes -- savesub.c's deserialize_settings() calls Copy_Save_w()
/// (sys_sub.c), which sets `mpp_w.language = save_w[1].Language` as part of
/// that load -- see the call site in src/sf33rd/Source/Game/init3rd.c. This
/// lets the MiSTer OSD's Language option (status bit [47]) win at boot, the
/// same config-overrides-save ordering BGM Type uses for status bit [14].
///
/// The key's shipped default is the `auto` sentinel (config.c
/// default_entries), which means "no override" -- the locale-derived
/// Get_Default_Language() / settings-save value stands. Before returning,
/// the resolved value is materialized back into the key via
/// Language_PersistToConfig() so the wrapper's OSD seed
/// (read_runtime_language_default(), thirdsarm_wrapper.cpp) has a concrete
/// value to show instead of guessing English for an `auto`/absent key.
void Language_ApplyBootOverride(void);

/// Two-way sync: call after the in-game Screen Adjust menu changes
/// mpp_w.language (menu.c's Screen_Exit_Check) so the on-disk `language`
/// key -- and therefore what the OSD seeds from at the next launch --
/// reflects the in-game choice. Best-effort / fire-and-forget, matching
/// BgmType_PersistToConfig() and the wrapper's own
/// write_runtime_*_default() helpers. No-ops when the key already holds
/// the requested value, so it is safe to call unconditionally.
void Language_PersistToConfig(Language language);

#endif
