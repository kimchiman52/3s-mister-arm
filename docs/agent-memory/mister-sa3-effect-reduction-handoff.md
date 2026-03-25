# SA3 Effect Reduction — SHIPPED

**Date:** 2026-03-25
**Branch:** `super-fidelity-ralph-loop`
**Status:** Background caching with scaled blit is working and deployed.

For full technical reference, see [mister-geneijin-rendering.md](mister-geneijin-rendering.md) — section "Background Caching Fix (SHIPPED — FRAME_SKIP quality mode)".

## What's Deployed

**Background caching** via `FRAME_SKIP` quality mode. On the first SA3 burst frame, background is rendered normally and a mid-render snapshot is saved. All subsequent burst frames restore the cached background via memcpy (or nearest-neighbor scaled blit during zoom animation), then render characters/effects/HUD fresh at 60fps. Background render tasks are dropped from the task array.

**Result:** ~60fps during Genei-Jin (up from ~45fps baseline). All visual effects intact. Zoom animation handled correctly via derived BgMATRIX transform formula.

## Remaining Cleanup

- `#if 0`-wrapped dead code from earlier per-effect classification attempts can be deleted (see "Legacy Code State" in geneijin-rendering.md)
- `classify_super_effect_hot_family()` is dead code — no longer called
- This handoff file can be deleted once cleanup is done
