#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void frame_trace_tick(void);

/* Freeze-boundary probe (fit.md §5): emit the engine's projectile-spawn
 * flag + super-freeze state once per game frame. Env-gated on
 * FD_SPAWN_PROBE (inert without it, including in the golden suite). Must
 * be called AFTER njUserMain() but BEFORE frame_data_overlay_tick() so it
 * reads fd_engine_proj_spawned pre-consume — see the definition comment. */
void frame_spawn_probe_tick(void);

void FrameTrace_SetPerfTickNs(uint64_t ns);
uint64_t FrameTrace_GetPerfTickNs(void);

/* Emit a free-form `# ...` annotation line into the trace file. Used by
 * frame_data_overlay to mark MOVE_START / FINAL events so the trace is
 * self-describing — no need for the captor to remember which buttons
 * were pressed or what numbers the overlay displayed. Opens the trace
 * file lazily if not yet open. No-op outside training mode. */
__attribute__((format(printf, 1, 2)))
void frame_trace_annotate(const char* fmt, ...);

#ifdef __cplusplus
}
#endif
