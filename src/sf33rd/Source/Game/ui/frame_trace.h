#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void frame_trace_tick(void);

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
