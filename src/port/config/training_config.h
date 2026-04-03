#ifndef TRAINING_CONFIG_H
#define TRAINING_CONFIG_H

#include <stdbool.h>

/// Load training settings from disk into Training[0] and Training[2].
/// If file is missing or corrupt, does nothing (caller should init defaults first).
/// Returns true if settings were loaded successfully.
bool TrainingConfig_Load(void);

/// Save current training settings (Training[2]) to disk.
void TrainingConfig_Save(void);

/// Restore saved character select cursor positions and SA selections.
/// Call before character select starts in training mode.
void TrainingConfig_RestoreCharSelect(void);

#endif
