#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void frame_data_overlay_tick(void);
void frame_data_overlay_draw(void);

/* FD_IDLE_PROBE (diagnostic): per-tick idle-ledger emitter for the D-f
 * adv ±1 residual investigation. Env-gated (FD_IDLE_PROBE) on top of the
 * frame-trace gates; strictly observation-only (reads g_cur/plw, writes
 * only to the trace log via frame_trace_annotate). Inert in normal play
 * and in the golden suite. Call AFTER frame_data_overlay_tick(). */
void fd_idle_probe_tick(void);

#ifdef __cplusplus
}
#endif
