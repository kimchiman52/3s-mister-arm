# SA Background Caching — SHIPPED (Universal)

**Date:** 2026-03-25
**Branch:** `super-fidelity-ralph-loop`
**Status:** Background caching with scaled blit is working and deployed. Now universal for ALL characters and ALL super arts.

For full technical reference, see [mister-geneijin-rendering.md](mister-geneijin-rendering.md) — section "Background Caching Fix (SHIPPED — CACHED_BG quality mode)".

## What's Deployed

**Background caching** via `CACHED_BG` quality mode. Detection uses `sa_stop_check()` which fires for ANY super art activation on ANY character. On the first activation frame, background is rendered normally and a mid-render snapshot is saved. Subsequent frames while the cinematic freeze is active (plus a ~15 frame grace period) restore the cached background via memcpy (or nearest-neighbor scaled blit during zoom animation), then render characters/effects/HUD fresh at 60fps. Background render tasks are dropped from the task array.

**Result:** ~60fps during super art activations (up from ~45fps baseline). All visual effects intact. Zoom animation handled correctly via derived BgMATRIX transform formula.

## Key Changes from v1 (Yun-SA3 only) to v2 (Universal)

- Detection: `sa_stop_check() != 0` replaces `My_char[0] == 3 && Super_Arts[0] == 2` + `plw[0].sa->ok == -1`
- Frame window: Signal-following with grace period replaces hardcoded 82-frame countdown
- Cache invalidation: `SDLGameRenderer_InvalidateSABgCache()` called on fresh activation edge to force new snapshot
- Scope: Both players' super arts detected (sa_stop_check covers both)
