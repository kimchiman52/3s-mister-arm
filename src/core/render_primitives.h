/* Adapter shim — giblet's PR #243 software renderer expects
 * "core/render_primitives.h" but our tree's primitives live at
 * include/rendering/primitives.h. This shim forwards. Verified safe:
 * upstream core/render_primitives.h is byte-identical to ours modulo
 * include guard. Vec3 / TexCoord underlying types match
 * (include/structs.h for Vec3 & TexCoord; include/rendering/primitives.h
 * for Quad / Sprite / Sprite2). See task brief for citations. */
#ifndef CORE_RENDER_PRIMITIVES_H
#define CORE_RENDER_PRIMITIVES_H

#include "rendering/primitives.h"

#endif
