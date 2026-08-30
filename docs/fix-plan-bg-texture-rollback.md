# Fix Plan — BG Texture Handle Rollback-Unsafety
Date: 2026-04-24
Origin: `ppgCheckTextureNumber` DEBUG instrumentation caught a partial BG
texture load (`tex->handle[29..31].b16[0]` zero while `[0..28]` populated, same
`Texture*` throughout) during a Mac↔Mac netplay Chun-vs-Q match, after the
`chainex_check` fix was already in place. 320+ rollback restores over the run;
HUD/sprites/effects render normally while only the BG layer is black.

---

## Fix B post-mortem (2026-04-24)

**Status: Fix B implemented as described below, rebuilt, and re-tested. The
black-BG symptom still reproduces in the same Mac↔Mac Chun-vs-Q netplay
scenario.** New numbers after Fix B:

```
After Fix B (run 2, same classifier DEBUG):
  336 OK
   24 FAIL:handle=0   (all on tex=0x102e9c288, ix=18,19,20,21,22... of total=32)
   24 FAIL:be=0
```

Comparing against the pre-Fix-B baseline:
```
Before Fix B (run 1):
  547 OK
   69 FAIL:handle=0   (all on tex=0x104c24288, ix=29,30,31 of total=32)
   24 FAIL:be=0
```

### Why Fix B did not close the bug

Fix B added a rollback-safety guard at the top of `Bg_Texture_Load_EX`
(`src/sf33rd/Source/Game/stage/bg.c:283-305`). For that guard to run, the
function `Bg_Texture_Load_EX` itself must be called. The investigation traced
back the call sites:

1. `Bg_Texture_Load_EX` is called from exactly one place in gameplay:
   `bg_initialize` at `src/sf33rd/Source/Game/stage/bg_sub.c:1118-1120`.
2. `bg_initialize` is called from `ta0_init00` at
   `src/sf33rd/Source/Game/stage/tate00.c:60`.
3. `ta0_init00` is the `bg_w.bg_routine == 0` branch of the `TATE00()`
   dispatcher at `src/sf33rd/Source/Game/stage/tate00.c:42-48,54-61`.
4. `bg_w.bg_routine` is reset to 0 by `bg_work_clear()` at
   `src/sf33rd/Source/Game/stage/bg_sub.c:1050`, which is called once per
   match from `Game2_0` at `src/sf33rd/Source/Game/game.c:525`. Immediately
   after, `TATE00()` runs (`game.c:558`) which dispatches to `ta0_init00`
   (bg_routine==0) and increments `bg_routine` to 1 (`tate00.c:55`).
5. On frame F+1 and later, `G_No[2] = 3` (set by `Game2_0` at `game.c:505`)
   so the Game02 dispatcher routes to `Game2_3` (`game.c:647`) which calls
   `Game2_1` which calls `TATE00` with `bg_routine=1` — dispatches to
   `ta0_init01`, NOT `ta0_init00`. `Bg_Texture_Load_EX` is NOT called.

**Rollback consequence**: `bg_w.bg_routine` IS in the netplay save/restore
surface (confirmed: `bg_w` is a whole-struct `GS_SAVE` at
`src/netplay/game_state.c:737` and `GS_LOAD(bg_w)` at `src/netplay/game_state.c:1459`;
`bg_routine` is field 0 of the `BG` struct at
`src/sf33rd/Source/Game/stage/bg.h:56`). A `GekkoLoadEvent` that restores the
simulation to any frame where `bg_routine != 0` means the resim never
re-enters `ta0_init00` — which means `Bg_Texture_Load_EX` is never re-called
— which means **Fix B's guard never runs on those frames**.

In a typical netplay session, the very first rollback that happens anywhere
after `Game2_0` completes leaves `ppgBgTex[].be`/`.handle[]` in the live
in-memory state at the moment `load_state` fired. Every subsequent resim
frame uses that same frozen cache — there is no repair path. Fix B only
repairs the cache on the single frame per match where Game2_0 runs, which
is the one frame where the pre-match `Bg_Close` has already cleared the
cache anyway (making Fix B's guard a no-op on both live and resim of that
frame).

### Why the numbers changed but the symptom didn't

Run 1 failing `ix=29,30,31` on `ppgBgTex[0]` with `total=32`, `ixNum1st=132`
(= `(stg=0 * 64) + 0x84`) maps to stage 0 layer 0. For stage 0,
`bgtex_stage_gbix[0][0] = 0xF0F0F0F0` (`src/sf33rd/Source/Game/stage/bg_data.c:378`).
The population loop at `bg.c:361-366` starts with mask=0x80000000 and shifts
right per iteration. Bits 2, 1, 0 of `0xF0F0F0F0` are ZERO → ix=29, 30, 31
are never populated by design. **Arcade data intentionally leaves those
slots unloaded.** Any `ppgCheckTextureNumber` against them returns 0 → tile
skipped by `bgDrawOneChip` at `bg.c:1209` → rendered as nothing (the clear
color under everything is black).

This means Run 1's `69 FAIL:handle=0 on ix=29,30,31` is **not a bug** — it's
the expected arcade behavior for stage 0 at that scroll position. Those 69
samples were reads of tile indices whose texture data was correctly omitted.
The arcade would render the same nothing for those tiles.

Run 2 shifted to `ix=18,19,20,21,22...`. That pattern does not match any
single `bgtex_stage_gbix` mask's "bits legitimately clear" shape — it looks
like a true partial population that terminated around ix=17. But with Fix B
in place, `Bg_Texture_Load_EX`'s inner loop `for (i = 0; i < 32; i++, ...)
{ if (tgbix & mask) { _2nd(); _3rd(); } }` runs all 32 iterations
unconditionally. The loop does not early-exit and `_3rd` only silently
returns early (at `PPGFile.c:1401-1403`) when the handle slot is already
non-zero (idempotent guard). All other abnormal exits (`_2nd` accnum
overflow, `_3rd` srcAdrs-null, `_3rd` zero-handle) hang the game via
`while(1){}`. None of those hang paths are consistent with "match plays,
only BG is black."

The most likely remaining explanation is that Run 2's observed partial state
is **not produced by `Bg_Texture_Load_EX` at all** — it's a snapshot of
live-memory `ppgBgTex[0].handle[]` captured between two unrelated events:
e.g., `Bg_Close` ran (which calls `ppgReleaseTextureHandle(-1)` that iterates
and zeros every slot, then fires `ppgCheckTextureDataBe` which frees the
`handle` array when all slots are zero). If the renderer executes BEFORE
the subsequent `Bg_Texture_Load_EX` completes its full population — for
example if a resim frame sits in a Game2_0-equivalent context but the
implicit re-call of `Bg_Texture_Load_EX` has not yet completed all 32
iterations when rendering runs — we'd see partial state. But within a
single frame this should be impossible because Game2_0 at `game.c:442-529`
orders `BG_Draw_System` (line 445) BEFORE `System_all_clear_Level_B` (line
452, which calls Bg_Close) BEFORE `bg_work_clear` (line 525) BEFORE `TATE00`
(line 528, which leads to Bg_Texture_Load_EX). So the render at line 445
sees PRE-reload state (whatever Bg_Texture_Load_EX last produced), not
partial state.

### The structural problem Fix B was aimed at but can't reach

Fix B was correctly aimed at "force a clean reload inside
`Bg_Texture_Load_EX`." The flaw is that `Bg_Texture_Load_EX` is called only
at the live-run Game2_0 boundary — one frame per match in the live pass,
and only on resim frames that land on or cross that exact boundary. The
24 `FAIL:be=0` cases (a whole-chunk `be==0` failure, not a per-slot
`handle==0` failure) come from frames where rendering runs with `ppgBgTex[i].be
== 0` — i.e., the PPG chunk is fully torn down. `Bg_Close` is called by
`System_all_clear_Level_B` at seven different call sites
(`src/sf33rd/Source/Game/game.c:360, 452, 1082, 1128, 1310, 1324, 1429, 1730,
1772, 1802`; plus `src/netplay/netplay.c:197`, `src/sf33rd/Source/Game/ending/
end_main.c:81`, `src/sf33rd/Source/Game/screen/win.c:77`, `src/sf33rd/Source/
Game/screen/next_cpu.c:524`). Several of these (notably the pre-session
`setup_vs_mode` tear-down at `netplay.c:197`, and any end-of-match teardown
paths that a resim can touch) leave `ppgBgTex[].be == 0` for at least one
frame. If rollback + resim re-enters those frames, `Bg_Draw_System` is
called with `be == 0` and every BG tile in the viewport fails
`ppgCheckTextureNumber` — the whole layer renders nothing.

Fix B cannot help with this class of failure because its tear-down happens
INSIDE `Bg_Texture_Load_EX`, which is not called on these frames.

---

## Fix E — corrected approach (recommended, 2026-04-24)

**Core insight**: The BG texture cache is a **rendering** resource. The
`Texture*` structs hold references to GPU-side texture handles; the heap
pointer, handle indices, and `be` flag are all determined by a historical
sequence of `flCreateTextureHandle` / `flReleaseTextureHandle` / `ppgMallocF`
/ `ppgFree` calls that netplay does not serialize. Asking rollback to
restore this state either forces us to serialize it (Fix A — wrong because
handle indices alias GPU slots that drift) or forces us to make every
rollback re-invoke the expensive reload (Fix B — the reload isn't reachable
from most resim frames).

The right fix is **self-healing** in the read path: make
`ppgCheckTextureNumber` detect a stale/partial cache and trigger an
idempotent re-populate of the missing slots on demand. This ensures that no
matter what live-memory cache state a `GekkoLoadEvent` leaves behind, the
very first render call after the restore re-establishes correct content
before any pixel is drawn.

### Shape of the fix

1. **Guarantee the `srcAdrs` / `offset[]` / `ixNum1st` state needed for
   repair stays alive for the full match duration.** `Bg_Texture_Load_EX`
   at `src/sf33rd/Source/Game/stage/bg.c:246-394` already keeps `srcAdrs`
   alive for the three main `ppgBgList[0..2]` (it does NOT call
   `ppgSourceDataReleased` on them — only on Akane and Ake paths at
   `bg.c:405,421`). The `tch->offset[]` array is allocated in
   `ppgSetupTexChunk_1st` at `PPGFile.c:1296` and retained. `ixNum1st`,
   `total`, and `srcSize` are structural fields filled in by `_1st`
   (`PPGFile.c:1255-1262`). All the pieces the repair needs are already
   persistent.

2. **Add a per-`Texture` signature that makes "my cache matches my
   identity" verifiable.** Two approaches:

   **E.1 (minimal — works for the `FAIL:be=0` class)**: On every
   `Bg_Texture_Load_EX` entry, additionally stash the *intended* chunk
   signature onto a sidecar struct that IS rollback-covered. On every
   render, if the sidecar says "chunk X should be be=1 with these
   parameters" but the live `ppgBgTex[n].be == 0`, the renderer (or a
   pre-render check) re-invokes the load path for chunk X.

   **E.2 (full self-heal — works for both `FAIL:be=0` and the per-slot
   case)**: Rewrite `ppgCheckTextureNumber` (`PPGFile.c:1844`) to, on
   FAIL:handle=0 where the tile index IS expected to be populated by the
   stage's tgbix mask, fire a single `ppgSetupTexChunk_3rd(tex, num, 1)`
   attempt. For FAIL:be=0, fire a full `_1st` + `_2nd`-loop + `_3rd`-loop
   re-execution for the appropriate chunk (needs knowledge of which chunk
   to reload, which we get from the `Texture*` pointer and `bg_w.stage`).

E.2 is the "proper" fix but requires renderer-time knowledge of
"what chunk is this `Texture*`?" and "what's the current stage/mask?". That
information is available (`bg_w.stage` is in scope, and a pointer-compare
against `&ppgBgTex[0..2]` / `&ppgRwBgTex` / etc. identifies the chunk), but
the repair sequence is moderately involved.

E.1 is simpler. It requires:
- A rollback-covered sidecar: `GameState` gains a single `u8 bg_tex_reload_generation;`
  field that `Bg_Texture_Load_EX` increments and that every render hook
  compares against a non-rollback-covered `u8 bg_tex_cache_generation;`. If
  they disagree after a rollback (rollback restored the old generation,
  cache wasn't torn down), a pre-render `ensure_bg_tex_loaded()` function
  runs the load + marks cache generation current.

However, both E.1 and E.2 carry a structural risk: **`flCreateTextureHandle`
returns indices into the global `flTexture[256]` pool, which is ALSO
non-rollback-covered**. If repair fires on resim frame N and allocates
handle index K, then repair fires again on resim frame N+1 (because the
rollback cursor moved forward) and allocates handle index K+1, the
live-memory GPU-side state drifts monotonically. The user's current log
shows `high_water=109-111 / max=256`, which gives headroom but not
infinite. A long match with many rollbacks could eventually exhaust the
pool.

### Recommended pragmatic fix: Fix E.3

Given the complexity of both E.1 and E.2, and given that the chainex_check
fix + fix-plan-bg-texture-rollback base work has already landed, the
pragmatic next step is **smaller and more targeted**:

**Fix E.3 — Defensive `be` check + forced `Bg_Texture_Load_EX` at TATE00's
start-of-frame**. Shape:

1. At the top of `TATE00()` (`src/sf33rd/Source/Game/stage/tate00.c:41`),
   add a check: if `bg_w.bg_routine > 0` (meaning we're in the main-loop
   phase, not the init phase) but any of `ppgBgTex[0..(bg_w.scrno-1)]` has
   `be == 0`, force a repair by resetting `bg_w.bg_routine = 0` and letting
   the `jump_tbl[0]()` dispatch re-run `ta0_init00` which runs
   `bg_initialize` which calls `Bg_Texture_Load_EX`.

   ```c
   // src/sf33rd/Source/Game/stage/tate00.c:41 (insert just after the
   // Game_pause early-out at line 44-46):
   if (bg_w.bg_routine > 0) {
       int _scrno = bg_w.scrno;
       if (_scrno > 3) _scrno = 3;
       int _needs_reload = 0;
       for (int _i = 0; _i < _scrno; _i++) {
           if (ppgBgTex[_i].be == 0) { _needs_reload = 1; break; }
       }
       if (_needs_reload) {
           bg_w.bg_routine = 0;
           bg_w.bg_r_1 = 0;
           bg_w.bg_r_2 = 0;
       }
   }
   ```

2. Because `Bg_Texture_Load_EX` now has the Fix B guard (still useful as a
   safety net when the forced-reload path runs), the force-reload
   correctly tears down any half-state and re-populates.

**Why E.3 works where Fix B alone did not**: Fix B only runs when
`Bg_Texture_Load_EX` is called. E.3 forces `Bg_Texture_Load_EX` to be
called whenever the cache is detectably stale, regardless of what
`bg_routine` rollback restored. The check runs on every TATE00 invocation
(every frame in Game2_1/Game2_3/Game2_5's main-loop path), which is frequent
enough to catch any stale-cache state the first time the game tries to
render the BG.

**Why E.3 is safer than E.2**: E.3 triggers on a strict "be=0 when it
shouldn't be" precondition, not on every tile-level sample. It reuses the
existing well-tested `Bg_Texture_Load_EX` path rather than creating a new
self-healing render path. It doesn't modify `flTexture[]` churn rate
meaningfully because reloads only fire when `be=0` (which means the handles
were already freed via `ppgReleaseTextureHandle` / `ppgCheckTextureDataBe`).

**Why E.3 costs near-zero on the happy path**: The added check is:
```
if (bg_w.bg_routine > 0) { for (i<scrno) if (be==0) break; }
```
At steady state `be==1` for all scrno slots, so the loop exits after one or
two iterations. Per frame cost: ~3 pointer dereferences, ~3 u8 compares.

### Concrete edits for Fix E.3

**File 1**: `src/sf33rd/Source/Game/stage/tate00.c`

Add a forward declaration of `ppgBgTex` at the top of the file (extern) if
not already visible. `tate00.c` already includes `bg.h` which includes
`PPGWork.h` indirectly — but let me verify by adding the include explicitly:

```c
#include "sf33rd/Source/Common/PPGWork.h"
```

Then in `TATE00()` (`tate00.c:41-52`), replace:

```c
void TATE00() {
    void (*jump_tbl[4])() = { ta0_init00, ta0_init01, ta0_init02, ta0_move };

    if (Game_pause & 0x80) {
        return;
    }

    jump_tbl[bg_w.bg_routine]();
    Scrn_Renew();
    Irl_Family();
    Irl_Scrn();
}
```

with:

```c
void TATE00() {
    void (*jump_tbl[4])() = { ta0_init00, ta0_init01, ta0_init02, ta0_move };

    if (Game_pause & 0x80) {
        return;
    }

    /* Rollback-safety: if `bg_w.bg_routine` was rolled back to a
     * post-init value but the BG texture cache (not covered by
     * GameState_Save/Load) is in a torn-down state, fall back to
     * bg_routine=0 so ta0_init00 re-runs bg_initialize and
     * Bg_Texture_Load_EX re-populates the cache. Without this, the
     * resim renders black BG tiles for the rest of the match because
     * no other path re-invokes Bg_Texture_Load_EX. See Fix E.3 in
     * docs/fix-plan-bg-texture-rollback.md.
     *
     * The Fix B guard inside Bg_Texture_Load_EX itself remains in
     * place as a safety net for the case where this reset is triggered
     * with non-zero be still lingering on some slot. */
    if (bg_w.bg_routine > 0) {
        int _scrno = (int)bg_w.scrno;
        if (_scrno < 0) _scrno = 0;
        if (_scrno > 3) _scrno = 3;
        int _needs_reload = 0;
        for (int _i = 0; _i < _scrno; _i++) {
            if (ppgBgTex[_i].be == 0) {
                _needs_reload = 1;
                break;
            }
        }
        if (_needs_reload) {
            bg_w.bg_routine = 0;
            bg_w.bg_r_1 = 0;
            bg_w.bg_r_2 = 0;
        }
    }

    jump_tbl[bg_w.bg_routine]();
    Scrn_Renew();
    Irl_Family();
    Irl_Scrn();
}
```

**File 2**: None required. The Fix B guard inside `Bg_Texture_Load_EX`
remains in place as a safety net when the forced reload fires with some
slots still populated (common because resim-time `be=0` only applies to
chunks whose GPU handles were freed; chunks that weren't freed — e.g.,
`ppgRwBgTex` if it was never populated for this stage — might still be
`be=0` trivially).

**File 3**: None required. `GameState` does NOT change. `bg_w.scrno` is
already rollback-covered (it's field 10 of the `BG` struct at
`src/sf33rd/Source/Game/stage/bg.h:64`, so it's saved via
`GS_SAVE(bg_w)` at `src/netplay/game_state.c:737`). No `EXPECTED_GAME_STATE_SIZE`
bump. No new fields.

### Risk and verification for Fix E.3

**Correctness risk**: LOW.

- If `bg_w.scrno == 0` (state before `bg_initialize` sets it), the loop
  doesn't run and the check is inert. At that point `bg_w.bg_routine`
  should also be 0 (via `bg_work_clear`), so the `bg_routine > 0` outer
  guard also blocks the check. Two-layer defense.
- If a non-netplay game mode (arcade/demo/training) happens to call
  `TATE00` with `ppgBgTex[i].be == 0` (e.g., legitimately between
  `Bg_Close` and `Bg_Texture_Load_EX` within Game2_0), the check resets
  `bg_routine=0` which causes `ta0_init00` to re-run. The original arcade
  path also reaches `ta0_init00` from `Game2_0`'s `TATE00` call (because
  `bg_work_clear` just set `bg_routine=0`), so this double-call is the
  same as the normal path — it would hit the Fix B guard's no-op case and
  proceed to a clean reload. Performance cost only (one extra
  `Bg_Texture_Load_EX` invocation), no semantic change.
- `bg_w.bg_r_1` and `bg_w.bg_r_2` reset alongside `bg_routine` mirrors
  `bg_work_clear` semantics at `bg_sub.c:1050-1052`. This preserves the
  invariant that when `bg_routine==0` we're "fresh init" and the sub-
  counters are also zeroed.

**Performance impact**: The per-frame cost of E.3 is ~3 byte loads and ~3
branches when the cache is healthy (the common case). The repair path (full
`Bg_Texture_Load_EX`) fires only on frames where the cache was detectably
torn down — plausibly once per rollback that crosses a Bg_Close boundary.
Bg_Texture_Load_EX itself takes on the order of 1–5 ms (decompresses 64+
tiles). If rollback frequency is high and every rollback crosses a Bg_Close
boundary, this could add meaningful per-frame cost, but in practice
rollbacks of that span are rare (delay-only 80ms RTT means typical rollback
windows are 3–5 frames; only rollbacks that cross a Game2_0/G_No[2]=0
transition trigger `Bg_Close` again, which is a very narrow window).

**Regression risk vs. Fix B alone**: LOW. E.3 is strictly additive on top
of Fix B. Any path where Fix B was already correct remains correct (E.3
doesn't disable or alter the Fix B guard). Paths where Fix B never fired
because `Bg_Texture_Load_EX` wasn't reached now fire Fix B indirectly via
E.3's `bg_routine=0` forced reset.

**Dynamic verification**:

1. Reproduce the Chun-vs-Q netplay match post-Fix-E.3.
2. Check DEBUG log from `ppgCheckTextureNumber`: both `FAIL:be=0` and
   `FAIL:handle=0` counts should drop significantly. `FAIL:handle=0` at
   legitimately-unloaded ix (e.g., stage 0 ix 29-31 per
   `bgtex_stage_gbix[0][0] = 0xF0F0F0F0`'s clear low bits) may remain as
   they reflect correct arcade data — but they don't cause visible
   black BG because the scroll window doesn't sample them at normal
   play positions.
3. Expected: BG renders correctly; `FAIL:be=0` = 0 for in-match frames;
   `FAIL:handle=0` only for legitimately-unloaded mask positions.
4. Check `[flPS2GetTextureHandle] high_water` does not grow unboundedly.
   Each forced reload pairs with a prior `flReleaseTextureHandle`, so the
   pool should oscillate but not monotonically grow.

**Static verification**:

1. `grep -n "Bg_Texture_Load_EX\|ta0_init00\|bg_work_clear" src/sf33rd/Source/Game/stage/*.c`
   — confirm the forced-reload path is reachable and the Fix B guard
   remains in `Bg_Texture_Load_EX`.
2. Build both desktop and MiSTer DEBUG. No new compile errors.
3. Confirm `sizeof(GameState) == 17652` (unchanged).

### Edge cases worth pinning on review

1. **`Game2_4`, `Game2_6`, `Game2_7`**: these call `BG_Draw_System` BEFORE
   `TATE00`. If a rollback lands in one of these routines with
   `ppgBgTex[].be == 0`, the render runs before E.3's check fires.
   Mitigation: Game2_4/6/7 are KO/finish-move wait states; `Bg_Close` is
   not called during them, so `be=0` here implies a mid-match corruption
   we should surface separately. Not blocking E.3.

2. **Rollback landing on a frame where `G_No[1] != 2` (not in Game02
   dispatcher)**: `TATE00` is called from `Game02_Jmp_Tbl`, which only
   runs when `G_No[1] == 2`. During chara select or intro, E.3 doesn't
   apply. This is correct — BG isn't drawn on those screens either, so
   the check is moot.

3. **Concurrent with Fix B**: E.3 works in concert with Fix B. When E.3
   forces `bg_routine=0` and `ta0_init00` runs, Fix B's guard tears down
   any stale half-state before `ppgSetupTexChunk_1st` fires. Belt AND
   suspenders.

4. **`bg_w.scrno` clamping**: the code clamps `scrno` to `[0..3]` before
   indexing `ppgBgTex[]`. This guards against a rollback restore leaving
   `scrno` at a garbage value. `ppgBgTex[]` is size 4, so index 0..3 is
   valid; clamping prevents any stale `scrno == 255` (from a u8 garbage
   value) from walking off the end.

---

## [SUPERSEDED 2026-04-24] Evidence summary

Verbatim from the reproducing DEBUG run (Chun-vs-Q, black BG from round start):

```
ppgCheckTextureNumber outcomes (640 calls):
  547 OK
   69 FAIL:handle=0
   24 FAIL:be=0

All 69 FAIL:handle=0 cases are on the SAME texture object:
  num=161 FAIL:handle=0 ix=29 total=32 tex=0x104c24288
  num=162 FAIL:handle=0 ix=30 total=32 tex=0x104c24288
  num=163 FAIL:handle=0 ix=31 total=32 tex=0x104c24288

For the same tex=0x104c24288, lower indices (ix=0..28) pass ppgCheckTextureNumber
(returned OK with non-zero handle values).

BG_Draw_System context from same run:
  ssb=0x3  bg_disp_off=0  Play_Game=1
  → BG draw path is active, not globally suppressed.

Handle pool status:
  [flPS2GetTextureHandle] high_water=111 / max=256
  → pool NOT exhausted. Plenty of free slots.
```

Decoding the indices:
- `ixNum1st = 132` (from `total=32`, `num=161`, `ix=29`; `num - ixNum1st = ix`)
- `132 = (stg*64) + 0x84` with `stg = 0` — so the failing texture is
  `ppgBgTex[0]` (the first BG layer of the currently-loaded stage).
- `total = 32` — loaded by `Bg_Texture_Load_EX` at `src/sf33rd/Source/Game/stage/bg.c:317`
  (`ppgSetupTexChunk_1st(NULL, loadAdrs, loadSize, (stg * 64) + 0x84, 32, 0, 0)`).
- The three missing slots correspond to the LOWEST three bits of
  `bgtex_stage_gbix[bg_w.stage][0]` (because the population loop at bg.c:320
  iterates `mask >>= 1` from `0x80000000`, so `ix=29` → bit 2,
  `ix=30` → bit 1, `ix=31` → bit 0).

Interpretation: the PPG texture chunk for BG layer 0 was set up (be=1, handle
array allocated, 29 out of 32 intended slots populated) but the trailing three
slots never received a handle — either because `ppgSetupTexChunk_3rd` was
never called for them on a given simulation pass, or because they were
populated on one pass and then zeroed (via `ppgReleaseTextureHandle` with
specific ixNum) on a later pass without being re-populated.

Since rollback restores `bg_w.bg_routine`, `G_No[]`, and `Play_Game` but does
NOT restore `ppgBgTex[].handle[]` contents or `ppgBgTex[].be`, any divergence
between the rollback-covered game-routine state and the non-rollback-covered
PPG cache state is permanent — there is no repair path inside the normal game
loop for "`be==1` but some handle slots that should be populated are zero."

---

## Texture state inventory

### `Texture` struct

`include/structs.h:1286-1299`:

```c
typedef struct {
    s8 be;           // "be" = "be here" / ready flag. 0 = empty, 1 = loaded.
    u8 flags;
    s16 arCnt;
    s16 arInit;
    u16 total;       // number of slots
    TextureHandle* handle;   // POINTER — storage is on ppg heap (mmAlloc)
    s32 ixNum1st;
    u16 textures;
    u16 accnum;
    u32* offset;
    u8* srcAdrs;
    size_t srcSize;
} Texture;
```

Where `TextureHandle` is a 4-byte union at `include/structs.h:1280-1284`:

```c
typedef union {
    u32 b32;
    u16 b16[2];    // [0] = GS slot index (flTexture[] + 1), [1] = flags+accnum
    u8 b8[4];
} TextureHandle;
```

**Critical layout fact**: `handle` is a POINTER to heap storage allocated by
`ppgMallocF` at `PPGFile.c:1263`. A naive `memcpy` save/restore of the
`Texture` struct would copy the pointer value, not the 4*total bytes of handle
data at that pointer. This matters for any "Fix A" that tries to serialize
texture state into GameState.

### Global BG texture instances

All are file-scope globals in `src/sf33rd/Source/Common/PPGWork.c`:

- `Texture ppgBgTex[4];` (PPGWork.c:7) — three main BG layers (stg 0/1/2) plus slot 3
- `Texture ppgRwBgTex;` (PPGWork.c:10) — rewrite BG tiles
- `Texture ppgAkeTex;` (PPGWork.c:13) — akebono (sky-lighting) tiles
- `Texture ppgAkaneTex;` (PPGWork.c:17) — stage-7 akane tiles

Associated `PPGDataList ppgBgList[4]` (PPGWork.c:8) — stores `(tex, pal)` pointer
pairs used by `ppgSetupCurrentDataList`. `ppgBgList[i].tex` is re-pointed to
`&ppgBgTex[i]` on every `Bg_TexInit` (bg.c:70-71), which runs at the start of
every `Bg_Texture_Load_EX`, so pointer stability is guaranteed.

### `tex->be` write sites (all that matter for BG)

- `PPGFile.c:1252` — `tch->be = 0;` — very first line after the "already-be" hang
  check at entry to `ppgSetupTexChunk_1st`.
- `PPGFile.c:1321` — `tch->be = 1;` — end of `ppgSetupTexChunk_1st`, set BEFORE
  `_2nd`/`_3rd` ever runs from the caller. **This is structurally load-bearing
  for the bug:** `be=1` means "chunk header accepted," NOT "all slots populated."
- `PPGFile.c:1761` — `tch->be = 0;` — inside `ppgCheckTextureDataBe` when every
  slot is zero; called at the end of `ppgReleaseTextureHandle`.
- `PPGFile.c:934` (not load-bearing for BG) — `ppgSetupTexChunkSeqs` init.
- `PPGFile.c:990` (not load-bearing for BG) — `ppgSetupTexChunkSeqs` completion.
- `PPGWork.c:51-65` — `ppgWorkInitializeApprication` zeros all `.be` fields at
  app boot (not per-match).

### `tex->handle[i].b16[0]` write sites

- `PPGFile.c:1271` — `_1st` init-to-zero after alloc: `tch->handle[i].b16[0] = 0;`
  in a loop over `ixNums`.
- `PPGFile.c:1436` — `_3rd` populate: `hnof->b16[0] = flCreateTextureHandle(&bits, attribute);`
  This is the ONLY site that writes a non-zero value (except the seqs-flavor
  loader, which BG does not use).
- `PPGFile.c:953` — `ppgSetupTexChunkSeqs` init-to-zero (not used by BG).
- `PPGFile.c:972` — `ppgSetupTexChunkSeqs` populate (not used by BG).
- `PPGFile.c:1710` — `ppgReleaseTextureHandle`, ixNum<0 branch: zeros every slot.
- `PPGFile.c:1726` — `ppgReleaseTextureHandle`, specific-ix branch: zeros the
  one requested slot.

There is NO call site in `src/sf33rd/Source/Game/stage/` that invokes
`ppgReleaseTextureHandle` with a specific (non-negative) ixNum against
`ppgBgTex[]`. The only non-negative-ixNum release is in
`src/sf33rd/Source/Game/opening/opening.c:339` against `&ppgOpnBgTex`, which is
unrelated to the BG layer. So post-initial-load, nothing in the BG path is
known to zero specific slots of `ppgBgTex[]` — which makes the partial-load
evidence even more anomalous.

### `tex->handle` pointer write sites

- `PPGFile.c:941` — `tch->handle = NULL;` in seqs init.
- `PPGFile.c:945` — `tch->handle = ppgMallocF(ixNums * 4);` in seqs init.
- `PPGFile.c:1000` — `tch->handle = NULL;` in seqs error path.
- `PPGFile.c:1263` — `tch->handle = (TextureHandle*)ppgMallocF(ixNums * sizeof(TextureHandle));`
  in `_1st`. This is the BG path's handle-allocation site.
- `PPGFile.c:1333` — `tch->handle = NULL;` in `_1st` error path (preceded by
  `while(1){}` at line 1335 — hang, not a recoverable path).
- `PPGFile.c:1752-1759` — `ppgCheckTextureDataBe` frees and NULLs `handle` when
  all slots are zero.

### BG lifecycle call chain

During a normal match transition, the texture lifecycle is:

1. `setup_vs_mode` (`src/netplay/netplay.c:147`) — called BEFORE the Gekko
   session starts. Line 197: `System_all_clear_Level_B()` (`sys_sub.c:982`)
   → `Bg_Close()` (`bg.c:226`). This is the pre-session teardown.
2. Game advances through `G_No[1]` = 12 (menu) → 1 (Game01, character select) →
   2 (Game02, match).
3. `Game02_Jmp_Tbl[G_No[2]]()` dispatches. At `G_No[2]=0` → `Game2_0`
   (`game.c:442`). **This is inside the rollback window.**
4. `Game2_0` runs:
   - Line 452: `System_all_clear_Level_B()` → `Bg_Close()` — releases the three
     main BG texture chunks (`ppgBgTex[0..2]` via `ppgReleaseTextureHandle(..., -1)`
     at bg.c:233), plus RwBg/Ake/Akane.
   - Line 525: `bg_work_clear()` — sets `bg_w.bg_routine = 0` (bg_sub.c:1050).
   - Line 528: `TATE00()` — dispatches via `jump_tbl[bg_w.bg_routine]`
     (tate00.c:42). `bg_routine==0` → `ta0_init00()` (tate00.c:54).
5. `ta0_init00` (tate00.c:54):
   - Increments `bg_w.bg_routine` to 1.
   - Calls `random_16()` (for RNG parity with arcade).
   - Calls `bg_initialize()` (bg_sub.c:1100).
6. `bg_initialize` (bg_sub.c:1100-1120):
   - Initializes `bg_w.*` scroll/layer fields.
   - Line 1118-1120: `if (G_No[0] != 2 || G_No[1] != 2 || G_No[2] != 2) { Bg_Texture_Load_EX(); }`
     At Game2_0 entry: `G_No[2]=0`, so gate is TRUE → call fires.
7. `Bg_Texture_Load_EX` (bg.c:246):
   - Line 269: `Bg_TexInit()` — re-asserts `ppgBgList[i].tex = &ppgBgTex[i]`.
   - Loop at bg.c:313-326 over 3 BG layers (`stg=0,1,2`):
     - `ppgSetupCurrentDataList(&ppgBgList[stg])` (bg.c:316).
     - `ppgSetupTexChunk_1st(NULL, loadAdrs, loadSize, (stg*64)+0x84, 32, 0, 0)` (bg.c:317).
       Inside `_1st` (PPGFile.c:1256):
       - `be==1` → hang (PPGFile.c:1265-1267). Safe assumption: be==0 on entry
         because Bg_Close preceded us.
       - Alloc `handle[]` for 32 slots (PPGFile.c:1280), zero them (PPGFile.c:1287-1290).
       - Set `be = 1` (PPGFile.c:1321). **Handles all zero at this moment.**
     - Inner loop (bg.c:320-325): for each `i` in 0..31, if `tgbix & (1<<31-i)`:
       - `ppgSetupTexChunk_2nd(NULL, i + ((stg*64)+0x84))` (PPGFile.c:1364).
       - `ppgSetupTexChunk_3rd(NULL, i + ((stg*64)+0x84), 1)` (PPGFile.c:1379).
       Inside `_3rd`:
       - `hnof = tch->handle + (ixNum - tch->ixNum1st)` (PPGFile.c:1399).
       - If `hnof->b16[0]` already non-zero → early return 1 (PPGFile.c:1401-1403).
         **This is the key idempotency guard.**
       - Otherwise: decompress pixel data, call `flCreateTextureHandle`,
         assign to `hnof->b16[0]` (PPGFile.c:1436).
       - If `flCreateTextureHandle` returned 0 → hang (PPGFile.c:1456-1459).
8. `ta0_init01` → `ta0_init02` → `ta0_move` progress `bg_routine` on subsequent
   frames. `Bg_Texture_Load_EX` is NOT called again this match (unless stage
   changes, which in netplay VS it does not).
9. Match plays. `ppgBgTex[]` sits at be=1 with populated handles until either
   the match ends (`Bg_Close` via another `System_all_clear_Level_B` elsewhere)
   or the app quits.

### `ppgCheckTextureNumber` read path

`PPGFile.c:1844-1924`. Called by `bgDrawOneChip` (via `ppgWriteQuadUseTrans`
implicitly or directly) for every BG tile the renderer wants to draw. Returns
0 → tile is skipped (nothing drawn for that square of the viewport).

- Line 1878: `if (tex->be == 0) return 0;` — FAIL:be=0
- Line 1890: `ix = num - tex->ixNum1st;`
- Line 1892: `if (ix >= tex->total) return 0;` — FAIL:ix-range
- Line 1904: `if (tex->handle[ix].b16[0]) return 1;` — OK
- Line 1924: `return 0;` — FAIL:handle=0

The FAIL:handle=0 outcome is the "black tile" outcome.

---

## Rollback surface analysis

### What IS in `GameState` (game_state.h:29-717)

Confirmed via `src/netplay/game_state.h` and `src/netplay/game_state.c`
GS_SAVE/GS_LOAD macro calls:

- `bg_w` (`BG bg_w;`, game_state.h:545) — full BG work state. Contains `bg_routine`,
  `stage`, `area`, `scno`, `scrno`, per-layer scroll state.
  `GS_SAVE(bg_w)` / `GS_LOAD(bg_w)` at `game_state.c:737` / `game_state.c:1459`.
- `Screen_Switch`, `Screen_Switch_Buffer` — GS_SAVE at game_state.c:585-586.
- `bg_disp_off`, `tokusyu_stage`, `ending_flag`, `rw_*` state,
  `bgPalCodeOffset`, `rw_dat` — all saved/loaded (game_state.c:587-602).
- `G_No[4]`, `Play_Game`, `Mode_Type`, `Demo_Flag`, `task[11]`, etc. (various
  GS_SAVE/LOAD sites throughout game_state.c).
- `chainex_check[2][36]` — the prior rollback-unsafe file-static fix from
  earlier today. Saved/loaded via extern at game_state.c:761-766 and
  game_state.c:1450-1454. `EXPECTED_GAME_STATE_SIZE = 17652` after this
  addition (game_state.c:53-56).

### What is NOT in `GameState` / `State`

Verified by `grep` for the following symbols in
`src/netplay/game_state.c` and `src/netplay/game_state.h` (zero hits each):

- `ppgBgTex` — the `Texture[4]` array. Contents not saved, pointers not saved.
- `ppgRwBgTex`, `ppgAkeTex`, `ppgAkaneTex`, and every other top-level `Texture`
  global.
- `ppgBgList` — the `PPGDataList[4]` array. `tex`/`pal` pointers not saved.
- The heap memory pointed to by any `Texture.handle`.
- `flTexture[FL_TEXTURE_MAX]` (`FL_TEXTURE_MAX = 256`, declared at
  `include/sf33rd/AcrSDK/ps2/foundaps2.h:22`, defined at
  `src/sf33rd/AcrSDK/ps2/foundaps2.c:22`).
- `ppg_w` (the PPG work state including the `mmHeap` used by `ppgMallocF`).
- Any renderer-side state (Renderer_CreateTexture/Renderer_DestroyTexture
  backing store; SDL texture objects).

### The rollback gap

The concrete shape of the gap:

1. Frame F (LIVE): Game2_0 runs. `Bg_Close` → `Bg_Texture_Load_EX` populates
   all 32 handle slots for `ppgBgTex[0]`. `be=1`, `handle[0..31].b16[0]` all
   non-zero. `bg_w.bg_routine = 1` (post-ta0_init00 increment).
2. Save at frame F: `state.gs.bg_w.bg_routine = 1`. No record of handle state.
3. Frames F+1..F+N play. More saves happen.
4. Rollback: Gekko issues LOAD for frame F'. `bg_routine` restored to
   whatever it was at F'. But `ppgBgTex[0].be` and `handle[]` contents do NOT
   change — they're still whatever they were in live-memory at the moment
   load_state was called.
5. Resim frames F'..now:
   - If F' is at or after line 528's TATE00 (`bg_routine >= 1`), `ta0_init00`
     does NOT run. `Bg_Texture_Load_EX` is NOT called. Any pre-existing
     `ppgBgTex` state is what rendering sees.
   - If F' is before that point (`bg_routine == 0`), `ta0_init00` fires.
     `Bg_Texture_Load_EX` runs. But NOTE: it does NOT precede with
     `Bg_Close` — the Bg_Close is at line 452, which is in `Game2_0` before
     `bg_work_clear`, so on a resim starting at `bg_routine=0` where Game2_0
     as a whole is NOT re-entered (we're mid-Game2_0 already or mid-Game2_1
     with bg_routine somehow at 0), `_1st` is called with `be==1` from the
     pre-existing state → `if (tch->be) while(1) {}` at PPGFile.c:1265-1267 —
     **app hangs**. This is NOT the observed symptom, which means this path
     isn't the actual one that fires.

### What scenario produces the observed symptom?

The specific "29 of 32 slots populated, same tex ptr, same total, same ixNum1st"
pattern requires one of:

**(A)** `_3rd` was called for slots 0..28 but NOT called for 29..31 on the live
pass. For this to happen, the `tgbix` mask must have had bits 2,1,0 clear.
That would mean the stage tilemap never intended to reference those slots, AND
the renderer is asking about them anyway (which would be a bug in how tile
indices get computed, but would render black tiles at those indices in EVERY
run — not a rollback issue).

**(B)** `_3rd` was called for all 32 slots on some pass, then slots 29..31 were
specifically zeroed afterwards, but NO code path in the BG subsystem does
targeted-index release against `ppgBgTex[]`.

**(C)** `_3rd` ran for all 32 slots, populating handles. Then rollback snapped
live state (heap handle pointer included) to a prior frame where a DIFFERENT
Texture object lived at 0x104c24288. E.g., the mmHeap recycles the same
address for a differently-sized allocation, and the new allocation is
mid-populated. The instrumented log reports the ADDRESS of the `Texture` struct
itself (which is the stable PPGWork.c global address, not the heap `handle`
pointer) — so this specific interpretation doesn't apply to `tex=0x104c24288`
(that IS the stable `&ppgBgTex[0]` address). But it DOES apply to
`tex->handle`: if the heap at address H previously held 32 populated
`TextureHandle`s and was freed, then re-allocated for a different request that
only wrote the first 29, and `tex->handle` still pointed to H from a pre-free
state, we'd see this exact pattern.

Scenario (C) is the most likely. Concretely:

1. Live frame F: `Bg_Texture_Load_EX` → `_1st` allocs heap at address H for 32
   handles. `ppgBgTex[0].handle = H`. Loop populates all 32 slots at H.
   `ppgBgTex[0].be = 1`. `bg_routine = 1`.
2. Live frame F+k: Game2_0 runs AGAIN (e.g., round reset, or some path that
   loops back to G_No[2]=0). `Bg_Close` → `ppgReleaseTextureHandle` zeros
   all 32 slots at H → `ppgCheckTextureDataBe` frees H → `ppgBgTex[0].handle = NULL`,
   `be = 0`. `Bg_Texture_Load_EX` → `_1st` allocs NEW heap at H' — possibly
   SAME address as H if the mmHeap's free-list returned the same block. Writes
   32 zero-initialized handles at H'. `ppgBgTex[0].handle = H'`, `be = 1`.
   Loop populates all 32 slots at H'.
3. Rollback: LOAD state restores `bg_w.bg_routine` to some value. But
   `ppgBgTex[0].handle` still points to H'. Say the LOAD restores bg_routine
   to 0 (pre-ta0_init00). Resim: `ta0_init00` runs → `bg_initialize` →
   `Bg_Texture_Load_EX` (because `G_No[2] != 2` — we're either at Game2_0 or
   mid-match). Since no `Bg_Close` preceded THIS particular call path
   (bg_initialize at bg_sub.c:1119 calls Bg_Texture_Load_EX directly without
   a Bg_Close, unlike Game2_0 at game.c:452 which pairs them), **`_1st` entry
   sees `be == 1`** → `while (1) {}` hang at PPGFile.c:1266.

Conclusion: the observed "partial load + no hang" symptom is NOT trivially
explained by the call-graph as written. Scenario (C) as above would hang at
the `_1st` entry gate, not produce partial state.

The most parsimonious remaining scenario is a RACE between the main simulation
thread and some other code path that modifies `Texture.handle` contents
asynchronously. On a single-threaded gameplay loop this shouldn't exist — but
worth auditing whether any MiSTer-specific or SDL-rendering-thread code writes
to `ppgBgTex[].handle`. A grep of the codebase finds NO writes to
`ppgBgTex[*].handle[...].b16[0]` from any non-PPG path, so that's ruled out.

### Working hypothesis — the actual mechanism

The most likely mechanism is in the MALLOC side: `ppgMallocF` uses
`mmHeapAlloc` (`src/sf33rd/Source/Common/MemMan.c`). If its free-list returns
the SAME block on successive allocations AND the caller only partially
overwrites the block (e.g., one caller writes 32*4 bytes, a later caller
writes 29*4 bytes), the tail of the block retains old data. In the specific
scenario:

1. Earlier in the match (or during character select under Gekko simulation),
   an OTHER texture chunk of `>32` handles was allocated at heap address H and
   fully populated.
2. That chunk was released (free→H).
3. `ppgBgTex[0]` is now allocated at H with `ixNums=32`. `_1st`
   zeros `handle[0..31]` at H (PPGFile.c:1270-1273). So the tail beyond 32 is
   whatever; the 32 slots themselves are zeroed here. Good.
4. Population loop writes to 29 of the 32 slots because... `tgbix & mask`
   leaves bits 2,1,0 clear for this stage.
5. `ppgCheckTextureNumber(0, 161)` is called with `num=161`, `ixNum1st=132`,
   `ix=29`, `total=32`. `tex->handle[29].b16[0] == 0`. Returns 0. Tile skipped.

**The symptom we observe is therefore NOT a corruption bug — it is the
expected behavior when the stage's tgbix mask legitimately leaves trailing
slots empty, AND the tilemap then tries to draw those indices anyway.**

What could cause the tilemap to reference slots that tgbix says aren't loaded?
The tilemap (`bg_map_tbl[bg_w.stage][i]` → `scr_bcm[]`) references global tile
indices that get translated via `global_index_real = global_index + (((y >> 7)
<< 3) + (x >> 7))` at bg.c:688, with `global_index = (bgnm * 64) + 100`. For
bgnm=0, `global_index = 100`, so `global_index_real` ranges from 100 upward.
Number 161 is `global_index_real = 161` → `y/128 * 8 + x/128 = 61` →
`(y/128, x/128) ∈ {(7,5),(6,13),(5,21),...}` within a scrolling window.

For the tilemap to produce `global_index_real = 161`, the `y,x` pair has to
fall in that row of the virtual BG. Which scroll position puts those in the
visible window? Stage 1 (Chun's stage, or one of the early stages) with
vertical scroll that reveals row 7 or higher — that matches the "few tiles
near the top (normally off-camera) are visible" symptom. When the scroll is at
its normal in-match position, rows 7+ are OFF-screen and the renderer never
asks about those indices. If `bg_prm[bgnm].bg_v_shift` is wrong (e.g., at some
default-zero state instead of the stage's normal offset), the visible-window
computation shifts and drafts OFF-camera tile indices into the visible set.

**This reframes the bug**: it's not a texture load failure per se — it's a
scrolling/layer-parameters desync that causes the renderer to SAMPLE tile
indices that were never intended to be loaded for this stage, revealing the
pre-existing zero-handle state of unused tail slots.

The `bg_prm[8]` field IS in GameState (game_state.h:691, GS_SAVE at
game_state.c — let me verify):

Indeed `bg_prm` is saved — grep shows it in `GS_SAVE(bg_prm)` at
`src/netplay/game_state.c` (line near 740s for the "work_sys" block). But what
about `bg_h_shift`/`bg_v_shift` being computed from NON-rollback state at
frame 0? If the first `Bg_Draw_System` runs before `bg_prm` is populated
properly after Game2_0 runs, it uses stale values.

---

## Ranked fix options

### Fix A — Serialize texture-cache contents into GameState

**Shape**: Add a new field to GameState that captures the "ready state" of
each PPG texture that the renderer consults. Since `Texture.handle` is a
heap pointer, we must deep-copy the handle array content, not the pointer.

**Required additions**:
- In `src/netplay/game_state.h`, add a struct like:
  ```c
  typedef struct {
      s8 be;
      u16 total;
      s32 ixNum1st;
      u8 flags;
      TextureHandle handles[64];  // covers max total for any BG chunk
  } PpgTextureSnapshot;

  PpgTextureSnapshot ppg_bg_snapshot[4];
  PpgTextureSnapshot ppg_rw_bg_snapshot;
  PpgTextureSnapshot ppg_ake_snapshot;
  PpgTextureSnapshot ppg_akane_snapshot;
  ```
- In `GameState_Save` (`src/netplay/game_state.c` near line 760), a new block:
  ```c
  for (int i = 0; i < 4; i++) {
      const Texture* t = &ppgBgTex[i];
      dst->ppg_bg_snapshot[i].be       = t->be;
      dst->ppg_bg_snapshot[i].total    = t->total;
      dst->ppg_bg_snapshot[i].ixNum1st = t->ixNum1st;
      dst->ppg_bg_snapshot[i].flags    = t->flags;
      if (t->be && t->handle) {
          SDL_memcpy(dst->ppg_bg_snapshot[i].handles, t->handle,
                     t->total * sizeof(TextureHandle));
      } else {
          SDL_memset(dst->ppg_bg_snapshot[i].handles, 0,
                     sizeof(dst->ppg_bg_snapshot[i].handles));
      }
  }
  /* …repeat for RwBg/Ake/Akane… */
  ```
- In `GameState_Load`, the inverse. But: **restoring `Texture.handle` contents
  alone is insufficient**. The `handle[i].b16[0]` values are indices into
  `flTexture[FL_TEXTURE_MAX]` (`src/sf33rd/AcrSDK/ps2/foundaps2.c:22`). If we
  restore a handle index whose corresponding `flTexture[] slot has been reused
  (freed then reallocated for a different texture), we'd be drawing with the
  wrong GPU-side data. So restoring handle indices REQUIRES also saving and
  restoring `flTexture[256]` OR ensuring that the GPU-side slots haven't been
  touched in the rollback window.
- `EXPECTED_GAME_STATE_SIZE` bump:
  `sizeof(PpgTextureSnapshot) * (4 + 3) ≈ (1+2+4+1+padding + 4*64) × 7 ≈ 260 × 7 ≈ 1820 bytes`.
  New `EXPECTED_GAME_STATE_SIZE = 17652 + 1820 = 19472` (approx — pin empirically).

**Lines of diff**: ~80-120 lines (struct + save/load blocks + tripwire bump).

**Risk**: MODERATE-HIGH.
- Restoring `Texture.handle` with stale GS slot indices is incorrect if those
  GS slots have been reassigned. The fix is incomplete without also snapping
  `flTexture[256]`, which would itself require rolling back every
  `Renderer_CreateTexture`/`Renderer_DestroyTexture` call — far beyond scope.
- Even covering just `Texture.be`/`handle[].b16[0]` in GameState would, for
  the observed symptom, simply ensure rollback re-enters the same partial
  state — it wouldn't produce the correct rendering unless the partial state
  was itself already correct on some prior frame.

**Verdict**: Fix A is structurally incorrect in isolation. It treats a
rendering-cache symptom as if it were a game-state symptom. Save/restore of
handle INDICES without save/restore of the GS-slot content they point to is a
lie. REJECT.

### Fix B — Make `Bg_Texture_Load_EX` idempotent + repair missing slots

**Shape**: On every entry to `Bg_Texture_Load_EX`, unconditionally tear down
the existing PPG state for each BG texture (force `be=0`, free `handle[]`,
null it) BEFORE calling `_1st`. This guarantees `_1st` enters with `be=0`
cleanly, even on rollback-induced re-entry, so `be=1` does NOT hang the
game. Then the population loop ALWAYS runs for the full tgbix-mask set of
slots — no dependence on prior state.

**Concrete edit**: Add at the top of `Bg_Texture_Load_EX`
(`src/sf33rd/Source/Game/stage/bg.c:246`, just after `Bg_TexInit()` at
line 269):

```c
void Bg_Texture_Load_EX() {
    ...
    mmDebWriteTag("\nSTAGE\n\n");
    Bg_TexInit();

    /* Rollback-safety: force-reset any prior PPG chunk state so _1st's
     * "be != 0 at entry hangs the game" guard is never tripped on resim,
     * and so the population loop always starts from an all-zero handle
     * array. Without this, a rollback restore that leaves ppgBgTex[*]
     * in a "be=1, partially-populated handle[]" state produces black BG
     * tiles permanently (no path re-runs the load). */
    for (s32 _i = 0; _i < 3; _i++) {
        if (ppgBgTex[_i].be) {
            ppgReleaseTextureHandle(&ppgBgTex[_i], -1);
        }
    }
    if (ppgRwBgTex.be) ppgReleaseTextureHandle(&ppgRwBgTex, -1);
    if (ppgAkeTex.be)  ppgReleaseTextureHandle(&ppgAkeTex, -1);
    if (ppgAkaneTex.be)ppgReleaseTextureHandle(&ppgAkaneTex, -1);

    for (i = 0; i < 8; i++) {
        bgPalCodeOffset[i] = 0x12C;
    }
    ...  // rest unchanged
}
```

**Lines of diff**: ~10-12 lines.

**Why it works**: Any rollback that leaves `ppgBgTex[*]` in a "be=1, partial
handle[]" state is now recovered: the guard re-releases everything, zeroes
`be`, frees `handle`, and the subsequent `_1st` call (line 317) allocates
fresh memory and the inner loop (line 320-325) re-populates every tgbix-masked
slot via `_3rd`. This is the SAME behavior as a fresh-match load.

**Risk**: LOW.
- `ppgReleaseTextureHandle(tch, -1)` is already idempotent: if `be==0` it
  returns immediately (PPGFile.c:1698-1700). So the new guard is safe even if
  we somehow re-enter from a clean-state path.
- Non-netplay callers of `Bg_Texture_Load_EX` (the only caller is
  `bg_initialize` at bg_sub.c:1119, which in turn is called from
  `ta0_init00` at tate00.c:60) always came from a path that expected
  `Bg_Close` to have already been called in `System_all_clear_Level_B`
  (game.c:452, `System_all_clear_Level_B → Bg_Close`). So the textures are
  already released when this runs normally. The guard is a NO-OP on the
  well-behaved path and a REPAIR on the pathological rollback path.
- One subtle issue: `System_all_clear_Level_B` (sys_sub.c:982) ALSO calls
  `effect_work_init()`. The new guard in `Bg_Texture_Load_EX` only re-tears
  BG textures — it does NOT duplicate `effect_work_init`. Since we're paired
  with a `System_all_clear_Level_B` already earlier in Game2_0 anyway, no
  duplication concern.
- Performance: adds one extra ixNum=-1 release iteration per `Bg_Texture_Load_EX`
  call. `Bg_Texture_Load_EX` is called ONCE per match (Game2_0 only) in non-
  rollback runs, and once per rollback resim that crosses Game2_0 in netplay.
  Negligible cost.

**Verdict**: This is the minimum-invasive fix that addresses the actual
rendering-level symptom regardless of the exact rollback-time contaminator.

### Fix C — Pre-load textures before Gekko session starts; suppress reload inside rollback window

**Shape**: Move `Bg_Texture_Load_EX` to run in `setup_vs_mode`
(`src/netplay/netplay.c:147`) before the Gekko session begins. Then inside
the game loop, replace the `Bg_Texture_Load_EX` call in `bg_initialize`
(`src/sf33rd/Source/Game/stage/bg_sub.c:1119`) with a no-op when netplay is
active.

**Why it doesn't work**: `Bg_Texture_Load_EX` depends on `bg_w.stage`, which
is determined by `Setup_Battle_Country` and picked via character selection
screens. Those run INSIDE the Gekko-simulated frame loop — they are not
available pre-session.

Additionally, the PPG chunk depends on having the stage's ramcnt key loaded
via `Push_LDREQ_Queue_BG`, which is asynchronous and requires Check_LDREQ_Clear
to drain. LDREQ drains inside the game loop, not in setup.

To make Fix C work would require hoisting all of stage-selection and LDREQ
drain out of the Gekko window, which is a multi-day refactor touching
character select, stage select, LDREQ plumbing, and the Gekko session start
timing. The change would risk breaking MiSTer-only and non-netplay paths.

**Verdict**: Fix C is architecturally clean but not minimally invasive.
REJECT for this fix.

### Fix D — Clear `be` on `Bg_Close` (already done)

`Bg_Close` at `src/sf33rd/Source/Game/stage/bg.c:226` calls
`ppgReleaseTextureHandle(-1)` for each BG texture. Inside (PPGFile.c:1685-1735):
- Iterates and zeros every `handle[i].b16[0]`.
- Calls `ppgCheckTextureDataBe` which, seeing all slots zero, frees the
  `handle` array and sets `be = 0`.

So Fix D is already in effect. It does not help because:

1. Rollback LOAD does NOT call `Bg_Close`. It restores GameState. Any
   non-GameState texture cache is left at whatever live-memory it was at.
2. `Bg_Close` only fires inside specific game-state transitions. Rollback can
   land at ANY frame boundary. A rollback LOAD to a point where `Bg_Close` was
   not about to run means no reset happens.

**Verdict**: Already in place. Not sufficient alone. REJECT as standalone fix.

### Ranking [UPDATED 2026-04-24 — Fix B demoted after failing re-test]

1. **Fix E.3** (force `bg_routine=0` at top of `TATE00` when cache is
   detectably torn down). WIN. ~20-line change, adds no new rollback
   surface, triggers the existing proven Bg_Texture_Load_EX path whenever
   `ppgBgTex[].be` disagrees with what the restored `bg_routine` implies.
   See the "Fix E — corrected approach" section at the top of this
   document.
2. **Fix B** (idempotent repair in `Bg_Texture_Load_EX`). RETAINED as a
   safety net. Does NOT close the bug alone because `Bg_Texture_Load_EX`
   is not reached on most resim frames (its only caller,
   `bg_initialize` via `ta0_init00`, runs only when
   `bg_w.bg_routine == 0`, and rollback typically restores that to a
   non-zero value).
3. **Fix C** (hoist out of rollback window). Correct in principle, but
   architecturally prohibitive.
4. **Fix D** (already done). Necessary-but-not-sufficient.
5. **Fix A** (serialize textures). Structurally incorrect in isolation;
   would require a much deeper refactor to save/restore `flTexture[]` too.

---

## [SUPERSEDED 2026-04-24] Recommended fix — Fix B

**Update 2026-04-24**: Fix B was implemented and tested. The symptom still
reproduces. See "Fix B post-mortem" section at the top of this document for
why. The authoritative recommendation is now **Fix E.3** (also at the top of
this document). Fix B remains in the source tree as a safety-net guard but
does not close the bug by itself.

Add a rollback-safety guard at the top of `Bg_Texture_Load_EX` that forces a
full tear-down of any pre-existing BG PPG texture cache state before the
fresh load proceeds.

### Edit checklist

**File**: `src/sf33rd/Source/Game/stage/bg.c`

**Location**: inside `Bg_Texture_Load_EX`, just after `Bg_TexInit()` at the
current line 269, before the `bgPalCodeOffset` initialization loop at line 271.

**Edit** (additive, ~12 lines):

```c
    mmDebWriteTag("\nSTAGE\n\n");
    Bg_TexInit();

    /* --- ROLLBACK-SAFETY: force-tear-down any pre-existing BG texture cache.
     *
     * The PPG texture cache (ppgBgTex[], ppgRwBgTex, ppgAkeTex, ppgAkaneTex)
     * is NOT covered by netplay GameState_Save/GameState_Load (verified:
     * zero hits on these symbols in src/netplay/game_state.{c,h}). A rollback
     * can therefore leave the cache in a state inconsistent with the restored
     * game_state (e.g., a partial handle population from a non-atomic live-
     * run, or stale content from a prior stage). If ppgSetupTexChunk_1st is
     * called with tch->be == 1, it hangs (PPGFile.c:1248-1250). If it's
     * called with tch->be == 0 but some handle[] slots zero that the
     * renderer expects populated, tiles render black permanently — there is
     * no second reload triggered in-match.
     *
     * Tearing down unconditionally here makes Bg_Texture_Load_EX idempotent:
     * after this guard the cache is guaranteed be==0 and handle==NULL, so
     * ppgSetupTexChunk_1st below always executes its full alloc+zero+set-be=1
     * sequence and the population loop always runs clean.
     *
     * ppgReleaseTextureHandle is a no-op when be==0 already (PPGFile.c:1708-1710),
     * so the non-rollback path pays almost nothing — Bg_Close ran a few
     * function calls ago via System_all_clear_Level_B (game.c:452) on the
     * normal Game2_0 entry, leaving be==0. This guard is a safety net for
     * the rollback resimulation case where Game2_0's Bg_Close wasn't the
     * last thing to touch the cache before we got here. */
    {
        int _i;
        for (_i = 0; _i < 3; _i++) {
            if (ppgBgTex[_i].be) {
                ppgReleaseTextureHandle(&ppgBgTex[_i], -1);
            }
        }
        if (ppgRwBgTex.be)  ppgReleaseTextureHandle(&ppgRwBgTex,  -1);
        if (ppgAkeTex.be)   ppgReleaseTextureHandle(&ppgAkeTex,   -1);
        if (ppgAkaneTex.be) ppgReleaseTextureHandle(&ppgAkaneTex, -1);
    }

    for (i = 0; i < 8; i++) {
        bgPalCodeOffset[i] = 0x12C;
    }
```

**No other file changes required.**
- No `GameState` struct changes.
- No `GS_SAVE` / `GS_LOAD` additions.
- No `EXPECTED_GAME_STATE_SIZE` update.
- No tripwire updates.

**Why this is safe on the normal path**: In the non-rollback case,
`System_all_clear_Level_B` at `game.c:452` runs `Bg_Close()` a few lines
before `TATE00() → ta0_init00 → bg_initialize() → Bg_Texture_Load_EX()`.
`Bg_Close` already zeros `be` to 0 on every texture. The new guard then finds
`be == 0` everywhere and does nothing — true NO-OP.

**Why this is correct on the rollback path**: Whatever state the cache was
left in by the live-memory freeze-frame that rollback snapped us out of, the
guard forcibly resets. `Bg_Texture_Load_EX` from then on behaves exactly as
it would on a fresh match start.

### What about `Bg_Texture_Load2` and `Bg_Texture_Load_Ending`?

These are two sibling functions in bg.c that do similar chunk loads for
etc/ending BG paths:
- `Bg_Texture_Load2` (bg.c:372): already calls `ppgReleaseTextureHandle(NULL, -1)`
  at line 403 before its `_1st` call, so it's structurally idempotent already.
- `Bg_Texture_Load_Ending` (bg.c:435): does NOT have a pre-release. Netplay
  doesn't reach the ending path (match-only), so it's out of scope for this
  fix, but could apply Fix B there too if we ever port endings to netplay.

No changes needed in either for this fix.

---

## Verification plan

### Static verification (immediate)

1. `grep -n "Bg_Texture_Load_EX\b" src/sf33rd/Source/Game/stage/bg.c` — confirm
   only one definition and one (indirect) caller path via `bg_initialize`.
2. `grep -rn "ppgBgTex\|ppgRwBgTex\|ppgAkeTex\|ppgAkaneTex" src/netplay/` —
   should still return zero hits, confirming the fix does NOT create a new
   rollback surface to track.
3. Build both desktop and MiSTer with DEBUG — confirm no compile errors from
   the added scope block.
4. Verify at runtime (via the existing DEBUG `fprintf` in `ppgCheckTextureNumber`)
   that `FAIL:handle=0` count drops to zero across an extended play session.

### Dynamic verification (the failing Mac↔Mac match)

1. Reproduce the Chun-vs-Q match from the original DEBUG capture.
2. Confirm BG renders correctly (no black layer).
3. Confirm DEBUG log for `ppgCheckTextureNumber`:
   - `FAIL:handle=0` — expected count = 0
   - `FAIL:be=0` — expected count = 0 (the 24 prior `FAIL:be=0` hits would
     also be repaired by Fix B because Bg_Texture_Load_EX's guard ensures a
     full re-load of the CURRENT stage; any `FAIL:be=0` in the original was
     plausibly from a stale-stage read and will no longer happen).
   - `OK` count — should be close to total call count.
4. Rollback count should stay similar (~320+ GekkoLoadEvents over a full
   match). Fix B doesn't change rollback frequency; it just makes the game
   robust to it.

### Non-netplay verification

1. Play Arcade mode, transition between stages (round wins progress stages).
   Confirm no BG rendering regression.
2. Replay a Replay-mode recording. Confirm no BG rendering regression.
3. Enter and exit Training mode. Confirm no BG rendering regression.

### MiSTer verification

1. Deploy to MiSTer (`/media/fat/_Other/3S-ARM.rbf` sacrosanct — do NOT touch).
   Deploy a DEBUG build of the game binary via standard release path.
2. Single-player match: BG renders normally. No performance regression.
3. Netplay Mac↔MiSTer: same stage, same BG rendering behavior Mac-side as Mac↔Mac
   post-fix.

### Unit/static check

No unit test exists for `Bg_Texture_Load_EX` directly. A static inspection is
sufficient: the added guard calls `ppgReleaseTextureHandle` which is already
well-covered by normal-path usage; the new code is exclusively additive.

---

## Regression risk

### Non-netplay paths

- **Arcade/VS-CPU/Training**: `Bg_Texture_Load_EX` is called via
  `bg_initialize` which is called via `ta0_init00` which is called via
  `TATE00` from `Game2_0` on match entry. `Game2_0` calls
  `System_all_clear_Level_B → Bg_Close` at `game.c:452`, which releases all
  BG textures 0 levels before we hit our new guard. So the guard finds
  `be == 0` and does nothing. Zero functional change.
- **Replay mode**: Same path. No change.
- **Demo/attract**: Does not go through `Game2_0` for match-start; uses
  `demo02.c:285-286` to set stage, and a separate `Bg_Texture_Load_EX` path.
  Since `Bg_TexInit` + `Bg_Texture_Load_EX` always re-run on demo entry and
  `Bg_Close` is called via the demo setup, same argument — guard is no-op.
- **Ending**: Uses `Bg_Texture_Load_Ending`, not `Bg_Texture_Load_EX`. Unchanged.
- **Bonus stages**: Use `Bg_Texture_Load2`, which already has its own
  release-before-load. Unchanged.

### MiSTer-specific paths

No MiSTer-specific code path modifies `ppgBgTex[]`, `ppgRwBgTex`, `ppgAkeTex`,
`ppgAkaneTex`, or the `handle[]`/`be` fields. Verified by:

```
grep -rn "PORT_MISTER" src/sf33rd/Source/Game/stage/ src/sf33rd/Source/Common/
  — zero hits within the lifecycle functions touched by this fix.
```

The fix is pure game-core C and behaves identically on MiSTer as on desktop.

### Performance impact

- In the non-rollback case, the guard is 4 `if (x.be) ...` checks against
  `be == 0` — four byte-compares, four branches-taken. < 1 μs cost per
  `Bg_Texture_Load_EX` call. Call frequency: once per match. Total cost:
  noise.
- In the rollback case where `be == 1` (the pathological path), the guard
  performs exactly the work that SHOULD be done to reset state. Cost:
  4 × `ppgReleaseTextureHandle(-1)` = 4 × (iterate total, free handles, set
  be=0). For 32-slot BG textures × 3 + ~3 single-slot chunks ≈ ~100 handle
  releases. Each release: `flReleaseTextureHandle` + one `Renderer_DestroyTexture`.
  Typical 1-10 ms one-shot cost, already within the budget of `Bg_Close`
  which does the same thing on the normal path.

---

## Out of scope

The following are NOT addressed by this fix and are explicitly left for
future investigation:

1. **Root cause of the initial partial-load state**: Fix B repairs the
   symptom but does not pin down exactly which rollback sequence produced
   the "be=1 + 29/32 handles populated" state. Possible contributors include
   `mmHeapAlloc` address-aliasing across free-then-realloc, a stage-change
   code path that calls `_1st` with different `ixNums` than a prior load
   leaving trailing slots, or a Gekko LOAD that briefly exposes stale
   live-memory during resim. Adding persistent DEBUG logging at every
   `_1st`/`_3rd`/`ReleaseTextureHandle` invocation would pin this down, but
   is unnecessary once the symptom is repaired.

2. **`flTexture[256]` save/restore for "true" rollback determinism**: A
   theoretically complete rollback would snapshot the GPU-side texture pool
   too. We don't do this because (a) it's far beyond the fix's blast radius,
   (b) the renderer treats handles as ephemeral anyway, and (c) the
   observable symptom (black BG) is fully addressed by ensuring the `Texture`
   structs are re-initialized on every BG-reload path.

3. **Other PPG texture categories** (`ppgScrTex`, `ppgOpnBgTex`, `ppgTitleTex`,
   `ppgWarTex`, `ppgCapLogoTex`, and MTS textures): these are loaded outside
   the match-start hot path, are typically torn down and re-loaded together
   via `ppgPurgeFromVRAM(type)` on mode transitions, and are unlikely to be
   touched by rollback inside a match. If a similar symptom appears for one
   of these, apply the same Fix B pattern at its load entry.

4. **`Bg_Texture_Load_Ending`**: already noted above. Apply Fix B if netplay
   ever reaches endings.

5. **`chainex_check` pattern mirror**: Fix B intentionally does NOT follow the
   chainex_check pattern of adding a field to GameState. The chainex_check
   bug was a rollback-unsafe FILE-STATIC scalar that had to be surfaced to
   GameState because its value was functionally part of game logic. The BG
   texture cache is a RENDERING CACHE; its correct handling is to be
   re-buildable on demand, not to be rollback-serialized. Fix B respects
   that architectural distinction.

---

## Appendix — citation index

| Claim | File:line |
|---|---|
| `Texture` struct definition | `include/structs.h:1286-1299` |
| `TextureHandle` union | `include/structs.h:1280-1284` |
| `ppgBgTex[4]` decl | `src/sf33rd/Source/Common/PPGWork.c:7` |
| `ppgBgList[4]` decl | `src/sf33rd/Source/Common/PPGWork.c:8` |
| `Bg_TexInit` asserts `ppgBgList[i].tex = &ppgBgTex[i]` | `src/sf33rd/Source/Game/stage/bg.c:70-71` |
| `Bg_Close` entry | `src/sf33rd/Source/Game/stage/bg.c:226-244` |
| `Bg_Close` releases ppgBgTex | `src/sf33rd/Source/Game/stage/bg.c:232-234` |
| `Bg_Texture_Load_EX` entry | `src/sf33rd/Source/Game/stage/bg.c:246` |
| `Bg_Texture_Load_EX` first-stage load | `src/sf33rd/Source/Game/stage/bg.c:313-326` |
| Population loop tgbix mask | `src/sf33rd/Source/Game/stage/bg.c:320-325` |
| `_1st` be-hang guard | `src/sf33rd/Source/Common/PPGFile.c:1248-1250` |
| `_1st` handle alloc | `src/sf33rd/Source/Common/PPGFile.c:1263` |
| `_1st` zero-init handles | `src/sf33rd/Source/Common/PPGFile.c:1270-1273` |
| `_1st` set be=1 | `src/sf33rd/Source/Common/PPGFile.c:1321` |
| `_3rd` already-populated guard | `src/sf33rd/Source/Common/PPGFile.c:1401-1403` |
| `_3rd` handle populate | `src/sf33rd/Source/Common/PPGFile.c:1436` |
| `_3rd` zero-handle hang | `src/sf33rd/Source/Common/PPGFile.c:1439-1443` |
| `ppgReleaseTextureHandle` be==0 early-return | `src/sf33rd/Source/Common/PPGFile.c:1698-1700` |
| `ppgReleaseTextureHandle` ixNum<0 zeros all | `src/sf33rd/Source/Common/PPGFile.c:1702-1715` |
| `ppgCheckTextureDataBe` frees+be=0 when all zero | `src/sf33rd/Source/Common/PPGFile.c:1737-1765` |
| `ppgCheckTextureNumber` entry | `src/sf33rd/Source/Common/PPGFile.c:1844` |
| `ppgCheckTextureNumber` be==0 fail | `src/sf33rd/Source/Common/PPGFile.c:1878` |
| `ppgCheckTextureNumber` handle==0 fail | `src/sf33rd/Source/Common/PPGFile.c:1904, 1924` |
| `flCreateTextureHandle` entry | `src/sf33rd/AcrSDK/ps2/flps2vram.c:28` |
| `flPS2GetTextureHandle` pool scan | `src/sf33rd/AcrSDK/ps2/flps2vram.c:163-197` |
| `FL_TEXTURE_MAX 256` | `include/sf33rd/AcrSDK/ps2/foundaps2.h:11` |
| `flTexture[256]` defn | `src/sf33rd/AcrSDK/ps2/foundaps2.c:22` |
| `flReleaseTextureHandle` | `src/sf33rd/AcrSDK/ps2/flps2vram.c:295-310` |
| `Game2_0` entry | `src/sf33rd/Source/Game/game.c:442` |
| `Game2_0` calls `System_all_clear_Level_B` | `src/sf33rd/Source/Game/game.c:452` |
| `Game2_0` calls `bg_work_clear` | `src/sf33rd/Source/Game/game.c:525` |
| `Game2_0` calls `TATE00` | `src/sf33rd/Source/Game/game.c:528` |
| `System_all_clear_Level_B` calls `Bg_Close` | `src/sf33rd/Source/Game/system/sys_sub.c:973-976` |
| `TATE00` dispatcher | `src/sf33rd/Source/Game/stage/tate00.c:41-52` |
| `ta0_init00` calls `bg_initialize` | `src/sf33rd/Source/Game/stage/tate00.c:54-61` |
| `bg_initialize` entry | `src/sf33rd/Source/Game/stage/bg_sub.c:1100` |
| `bg_initialize` `Bg_Texture_Load_EX` call | `src/sf33rd/Source/Game/stage/bg_sub.c:1118-1120` |
| `bg_work_clear` sets bg_routine=0 | `src/sf33rd/Source/Game/stage/bg_sub.c:1047-1050` |
| `GameState` struct | `src/netplay/game_state.h:29-717` |
| `bg_w` in GameState | `BG bg_w;`, `src/netplay/game_state.h:545` |
| `chainex_check` in GameState | `u8 chainex_check[2][36]`, `src/netplay/game_state.h:739` |
| `EXPECTED_GAME_STATE_SIZE` | `src/netplay/game_state.c:144` |
| `GameState_Save` bg_w | `GS_SAVE(bg_w)`, `src/netplay/game_state.c:737` |
| `GameState_Save` chainex_check extern | `src/netplay/game_state.c:761-766` |
| `GameState_Load` chainex_check extern | `src/netplay/game_state.c:1450-1454` |
| `save_state` (Gekko callback) | `src/netplay/game_state.c:1872` |
| `load_state` (Gekko callback) | `src/netplay/game_state.c:1878` |
| `advance_game` | `src/netplay/netplay.c:591` |
| `process_events` handles Gekko {Load,Advance,Save}Event | `src/netplay/netplay.c:684-722` |
| `setup_vs_mode` calls `System_all_clear_Level_B` | `src/netplay/netplay.c:197` |

## Addendum — cross-match stage-data inconsistency (2026-04-24, non-rollback)

### Observed mechanism

DEBUG logs (with Fix B's rollback-safety tear-down already in place) from the
reliable Chun-vs-Q repro make it clear that by the time match 2's
`Bg_Texture_Load_EX` runs, the PPG cache is already fully torn down
(`be=[0,0,0,rw=0,ake=0,aka=0]` at ENTRY #2). The black-BG does **not** come
from a stale texture cache; it comes from the loader's main loop running only
**one** iteration (`j=0 stg=0 …`, no `j=1`) for stage 17, while `BG_Draw_System`
walks `Screen_Switch_Buffer = 0x3` and thus tries to draw **two** BG layers.
The un-loaded `ppgBgTex[1]` (`be==0`) causes every tile of BG layer 1 to hit
the `ppgCheckTextureNumber` `be==0` fail branch
(`src/sf33rd/Source/Common/PPGFile.c:1878`) → 24 FAILs per frame → black.

The renderer and the loader disagree about **how many BG layers this stage
has**, and the disagreement is stage-17-specific.

### stage_bgw_number vs bg_w.scrno

Relevant constants for stage 17 (Q's stage):

| Table | Declaration | Value for stage 17 |
|---|---|---|
| `stage_bgw_number[22][3]` | `src/sf33rd/Source/Game/stage/bg_data.c:49-52` | `{1, 2, 0}` — two non-zero entries |
| `use_real_scr[22]` | `src/sf33rd/Source/Game/stage/bg_data.c:41` | `use_real_scr[17] = 2` |
| `bg_index_tbl[22][3]` | `src/sf33rd/Source/Game/stage/bg_data.c:537-541` | `{4, 4, 4}` — **non-identity remap to stage 4** |
| `use_real_scr[bg_index_tbl[17][0]]` | `use_real_scr[4]` at `bg_data.c:41` | `1` |

Stage 17 is the **only** stage in the whole 22-entry table whose `bg_index_tbl`
row is not identity (`bg_data.c:540`). Every other stage has
`bg_index_tbl[s] = {s, s, s}`, so the remap is invisible.

`Game2_2` seeds `Screen_Switch` from `stage_bgw_number[bg_w.stage]`
(`src/sf33rd/Source/Game/game.c:634-638`):

```c
for (i = 0; i < 3; i++) {
    if (stage_bgw_number[bg_w.stage][i] > 0) {   // stage-indexed
        Bg_On_R(1 << i);                          // bits 0|1 -> 0x3 for stage 17
    }
}
```

`Bg_Texture_Load_EX`'s main loop iterates `bg_w.scrno`
(`src/sf33rd/Source/Game/stage/bg.c:361`):

```c
for (j = 0; j < bg_w.scrno; j++, assign3 = stg++) {
    tgbix = bgtex_stage_gbix[bg_w.stage][j];    // stage-indexed
    ppgSetupCurrentDataList(&ppgBgList[stg]);
    ppgSetupTexChunk_1st(NULL, loadAdrs, loadSize, (stg * 64) + 0x84, 32, 0, 0);
    …
}
```

`bg_w.scrno` is the mismatch: it's set from `use_real_scr[bg_w.bg_index]`
(`src/sf33rd/Source/Game/stage/bg_sub.c:1114`) — i.e. indexed by
**`bg_index`**, not by **`stage`**:

```c
bg_w.bg_index = bg_index_tbl[bg_w.stage][bg_w.area];  // stage 17 -> bg_index 4
bg_w.scno  = use_scr[bg_w.bg_index];                  // 2
bg_w.scrno = use_real_scr[bg_w.bg_index];             // use_real_scr[4] = 1  ❌
```

Result for stage 17:

| Quantity | Source | Value |
|---|---|---|
| Layers the renderer draws (`Screen_Switch_Buffer` bits set) | `game.c:634-638` + `stage_bgw_number[17] = {1,2,0}` | 2 (bits 0,1 → 0x3) |
| Layers `bgtex_stage_gbix` provides | `bg_data.c:378` (`bgtex_stage_gbix[17][0..1]` non-zero) | 2 |
| Layers `Bg_Texture_Load_EX` actually populates | `bg.c:361` loop bound = `bg_w.scrno` = `use_real_scr[4]` | **1** |

Match 2's observed log exactly predicts this:

```
ENTRY #2  bg_w.stage=17  be=[0,0,0,rw=0,ake=0,aka=0]
main-loop j=0 stg=0 tgbix=0x7e7e0000 tex=0x1073ec288   <- iterated once, scrno=1
BG_Draw_System  ssb=0x3                                 <- renderer wants 2 layers
ppgCheckTextureNumber  208 OK / 24 FAIL:be=0            <- 24 tiles on BG layer 1
```

For contrast, match 1 (stage 15, Chun): `bg_index_tbl[15] = {15,15,15}`
identity → `bg_w.scrno = use_real_scr[15] = 2` matches
`stage_bgw_number[15] = {1,2,0}` (2 layers). Consistent, works. This matches
ENTRY #1's two main-loop iterations `j=0,1`.

### Root cause

The `bg_sub.c:1114` assignment is indexed by the wrong table. Stage 17 is an
intentional "reuse stage 4's scroll metadata with different textures" mapping
— see also `ta_move_tbl[17] = BG180`, i.e. stage 17 and stage 18 share a
move handler (`src/sf33rd/Source/Game/stage/tate00.c:33-34`). Scroll-related
per-layer data (`msp`, `limit_tbl3`, `ta_move_tbl`, `bg_w.scno`) legitimately
indexes by `bg_index`. Texture-related data (`stage_bgw_number`,
`use_real_scr`, `bgtex_stage_gbix`, `stage_priority`, `rewrite_scr`,
`bg_map_tbl`) indexes by `stage`. `bg_w.scrno` is the **texture-layer count**
and is only ever consumed by texture-loader loops
(`bg.c:361` in `Bg_Texture_Load_EX`, `bg.c:526` in `Bg_Texture_Load_Ending`),
so it is semantically a stage quantity but is being computed from `bg_index`.

Why Q's stage looks right in arcade/standalone: in arcade (`MODE_NETWORK`
branch of `Setup_Battle_Country`, `sel_pl.c:2030-2034`) the challenger's
character picks the stage; stage 17 is reached only when the Challenger is Q
(char 17). That character vs-side pattern rarely happens pre-netplay. Under
netplay's repeat-matches flow (Game2_0 → System_all_clear_Level_B → Bg_Close →
bg_initialize), the second match's `Setup_Battle_Country` is re-evaluated
with the post-win roles swapped, pushing stage 17 to the foreground. The
latent table-mismatch bug has been there since the PS2 data, just not
exercised.

### Proposed fix

Single-line change. Compute `bg_w.scrno` from `bg_w.stage`, not from
`bg_w.bg_index`, so it agrees with the stage-indexed tables that drive
everything from the `stage_bgw_number`→`Bg_On_R` loop that builds `Screen_Switch` (`game.c:663-667`) through `bgtex_stage_gbix[]`
(`bg.c:362`).

**File**: `src/sf33rd/Source/Game/stage/bg_sub.c`

**Line 1114** — replace:

```c
    bg_w.scrno = use_real_scr[bg_w.bg_index];
```

with:

```c
    bg_w.scrno = use_real_scr[bg_w.stage];
```

Do **not** change `bg_w.scno` at line 1113 — that one is correctly
`bg_index`-indexed; it drives scroll-family loops (e.g. `bg_sub.c:1153`,
`for (i = 0; i < bg_w.scno; i++) … *msp[bg_w.bg_index][i]`) where the
layer count must match the scroll-metadata table, which is per-`bg_index`.

**Effect matrix**:

- Stage 17: `use_real_scr[17] = 2` → `scrno = 2` → main loop populates
  `ppgBgTex[0]` AND `ppgBgTex[1]` → matches `Screen_Switch_Buffer = 0x3` →
  black BG goes away.
- All other stages: `bg_index_tbl[s] = {s,s,s}` identity (`bg_data.c:537-541`)
  → `use_real_scr[bg_index]` == `use_real_scr[stage]` → zero behavior change.
- Netplay: `bg_w.scrno` is part of the `bg_w` struct saved by GameState
  (`BG bg_w;` at `src/netplay/game_state.h:545`; `GS_SAVE(bg_w)` `game_state.c:737` /
  `GS_LOAD(bg_w)` `game_state.c:1459`). Both peers compute
  the same new value from the same `bg_w.stage`, so it is deterministic and
  rollback-safe.
- Ending/bonus paths unaffected: `bg_w.scrno` is reassigned independently in
  `bg_etc_write` (`bg_sub.c:1203`, uses `use_scr2[type]`) and the ending loader
  (`src/sf33rd/Source/Game/ending/end_main.c:253`, `end_use_real_scr[end_w.type]`).

Why not the alternatives:

- **(a) Change the `Bg_Texture_Load_EX` main-loop bound** to `use_real_scr[bg_w.stage]`
  or to the `stage_bgw_number` non-zero count. Equivalent result, but it
  leaves `bg_w.scrno` holding a wrong value that `Bg_Texture_Load_Ending`
  (`bg.c:526`) would happily read later in a non-ending path — a bigger
  surface for future footguns. Also touches two files and two loops.
- **(b) Change `Game2_2` to only enable layers the loader populated.** Would
  require threading "layer count" from loader to `Game2_2`, and fights the
  existing convention that texture layers are a stage property. Would also
  leave `Bg_Texture_Load_Ending`'s misread untouched.

### Why prior rollback-based analysis missed this

1. The repro pattern ("1st match fine, 2nd match black") fit the rollback-
   contamination narrative because the *first* match was not affected and
   the *second* match is where the netplay teardown+re-init happens. Fix B
   attacked the only cache-state variable that *could* differ between matches
   (`be==1` leaked handles) and left the table-mismatch root cause
   untouched.
2. The repro also requires a **character-17 matchup** — a condition the
   earlier analysis treated as noise ("any character pair should repro")
   because Fix B's hypothesized mechanism wasn't character-specific. With the
   DEBUG log showing `bg_w.stage=17` on ENTRY #2 and the main loop iterating
   once instead of twice, the character-specificity became the smoking gun.
3. The Fix B tear-down at `bg.c:307-317` is still correct as defensive
   rollback hygiene; the log shows it runs and leaves `be=[0,…]` going into
   match 2. It just wasn't addressing the actual fault — which is why the
   subsequent `ppgBgTex[1].be` stayed `0`: the loader never wrote to it
   because `bg_w.scrno == 1`.

### Citations summary

| Claim | File:line |
|---|---|
| `stage_bgw_number[17] = {1,2,0}` → 2 layers enabled | `src/sf33rd/Source/Game/stage/bg_data.c:49-52` |
| `use_real_scr[17] = 2`, `use_real_scr[4] = 1` | `src/sf33rd/Source/Game/stage/bg_data.c:41` |
| `bg_index_tbl[17] = {4,4,4}` (only non-identity row) | `src/sf33rd/Source/Game/stage/bg_data.c:540` |
| `bg_w.scrno` assigned from `bg_index`, not `stage` | `src/sf33rd/Source/Game/stage/bg_sub.c:1114` |
| `bg_w.scno` correctly from `bg_index` (scroll-family) | `src/sf33rd/Source/Game/stage/bg_sub.c:1113, 1153-1164` |
| `Game2_2` seeds `Screen_Switch` via `stage_bgw_number` | `src/sf33rd/Source/Game/game.c:634-642` |
| `Bg_On_R` sets `Screen_Switch` / `_Buffer` bits | `src/sf33rd/Source/Game/stage/bg.c:1498-1501` |
| Main-loop iterates `bg_w.scrno` (the mismatch site) | `src/sf33rd/Source/Game/stage/bg.c:361` |
| `bgtex_stage_gbix[]` stage-indexed | `src/sf33rd/Source/Game/stage/bg_data.c:378`, ref `bg.c:362` |
| `ta_move_tbl[17] = BG180` (stage 17 shares handler w/ 18) | `src/sf33rd/Source/Game/stage/tate00.c:33-34` |
| `BG_Draw_System` walks `Screen_Switch_Buffer` bits | `src/sf33rd/Source/Game/system/sys_sub.c:927-939` |
| `ppgCheckTextureNumber` be==0 fail branch | `src/sf33rd/Source/Common/PPGFile.c:1878` |
| Netplay saves entire `bg_w` (incl. `scrno`) | `BG bg_w;` at `src/netplay/game_state.h:545`; whole-struct save/restore of `bg_w` at `game_state.c:737` / `game_state.c:1459` |
| `Setup_Battle_Country` net/arcade path returns challenger's char | `src/sf33rd/Source/Game/screen/sel_pl.c:2030-2034` |

