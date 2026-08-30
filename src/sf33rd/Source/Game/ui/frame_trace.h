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

/* Task #108: one row per effect_A5_move() entry, recording whether the
 * select-timer runner ran or hit its Present_Mode 4/5 early return, and what
 * the countdown state was. Env-gated on FD_SELECT_PROBE (the OUTPUT PATH);
 * completely inert without it, including in the golden suite. Deliberately
 * NOT routed through frame_trace_annotate(), which is training-mode-gated --
 * see the definition comment. Observation only. */
void frame_select_timer_probe(int present_mode, int routine_no, int unit_of_timer, int select_timer, int early_return);

#ifdef __cplusplus
}
#endif
