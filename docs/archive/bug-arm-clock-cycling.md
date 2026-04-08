# Bug: ARM clock cycling in OSD menu doesn't reliably apply frequency

## Symptom

1200MHz works great initially (heavy stages stable at 60fps). After cycling
through other speeds in the OSD menu and back to 1200MHz, performance is
worse in the same scenario.

## Architecture

- **Wrapper** (`threesx_wrapper.cpp`) handles OSD menu, cycles `g_wrapper_arm_clock`
- On cycle: writes config file via `write_runtime_arm_clock_default()`, sends `SIGRTMIN+2` to game
- **Game** (`main.c` signal handler) sets flag, next frame calls `SDLApp_CycleArmClock()`
- `SDLApp_CycleArmClock()` (`sdl_app.c:9142-9182`) re-reads config, calls `apply_arm_clock(mode)`
- `apply_arm_clock()` (`sdl_app.c:9101-9112`) writes to `/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq`

## Root cause

`apply_arm_clock()` uses buffered `fprintf` to write to sysfs with no error
checking and no read-back verification:

```c
FILE* f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "w");
if (f != NULL) {
    fprintf(f, "%s\n", freq);  // return value ignored
    fclose(f);                  // return value ignored
}
```

After cycling through intermediate frequencies (especially down to 800MHz
stock), the subsequent write to raise `scaling_max_freq` back to 1200000
may silently fail.

## Contributing factors

1. **Buffered I/O** -- `fprintf` to `FILE*` is buffered; sysfs writes are
   more reliable with direct `write()` syscall
2. **No error checking** -- `fprintf` and `fclose` return values ignored
3. **No verification** -- never reads back `scaling_max_freq` to confirm
   the kernel actually applied the change
4. **Governor latency** -- `ondemand` governor has sampling delay; after
   being capped at 800MHz, it may not ramp up instantly even if the write
   succeeds

## Proposed fix

1. Use `open()`/`write()`/`close()` (unbuffered) instead of `fprintf` for sysfs
2. Check return values
3. Read back `scaling_max_freq` after writing to verify the frequency was applied
4. Log a warning if the read-back doesn't match the requested frequency

## Files

| File | Lines | What |
|------|-------|------|
| `vendor/Main_MiSTer/threesx_wrapper.cpp` | 1400-1409 | OSD menu cycle logic |
| `vendor/Main_MiSTer/threesx_wrapper.cpp` | 1850-1862 | Post-exit clock reset |
| `src/port/sdl/sdl_app.c` | 9101-9112 | `apply_arm_clock()` -- the buggy write |
| `src/port/sdl/sdl_app.c` | 9142-9182 | `SDLApp_CycleArmClock()` -- re-reads config |
| `src/main.c` | 128-129, 749-752 | Signal handler + main loop dispatch |
