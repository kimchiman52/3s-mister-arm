# 3SX — Arcade (CPS3) ROM Data Accuracy: Research, Root Cause & Worklist

**Date:** 2026-08-29
**Repo:** `/Users/sb/Developer/3sx-mister`
**Branch examined:** `upstream-engine-fixes`
**Upstream compared:** `crowded-street/3sx` @ `513380f9` ("Refactor texgroup (#372)")
**Tooling:** `tools/arcade-audit/` (this repo, same branch)

This is a **handoff document**. It is written so the work can resume cold, with
no prior conversation context. Every factual claim is traced to a `file:line`, a
command and its observed output, or a named primary source. Things that were
*not* verified are called out in §12 rather than smoothed over.

---

## 1. How to use this document

- **Just want the state of play?** §2 and §3.
- **Fixing the crash?** §5 (root cause) then §8 (worklist item A).
- **Fixing wrong sprites?** §7 (audit results) then §8 (items B-E).
- **Need to re-run the analysis?** §9 (tooling) — it works today, verified.
- **Need to reproduce the crash live?** §10 (exact build + run recipe).
- **Wondering what we *can't* find statically?** §11.

---

## 2. TL;DR

1. **The reported bug is fixed-shaped and understood.** Upstream issue #363
   ("Ryu's Denjin Hadoken crashes the game") was reproduced under
   AddressSanitizer and root-caused. It is **not** a Ryu bug: the out-of-range
   sprite index lives in the **victim's** damage-reaction table. In the crash
   replay the victim is **Elena**.
2. **The defect class is now exhaustively enumerated.** An audit of all 20
   characters × 10 script tables (**133,901 cells**) against both the decrypted
   CPS3 ROM and the PS2 AFS found the crash class is **Elena-only: 66 cells at
   3 sites**. Nobody else in the cast can produce an out-of-range sprite index.
3. **A second, larger class exists: 1,694 wrong-sprite cells** (in-range but
   mismatched against PS2). These are cosmetic, not crashes, and are
   concentrated in Makoto (521), Ibuki (408), Urien (256), Twelve (88) and a
   13-character cross-bank cluster.
4. **The root mechanism is a modelling limitation, not a typo.** `remap_cg_number`
   models CG translation as *one delta per character* plus a few patch ranges.
   The data contains cross-bank references and per-script deltas that a single
   delta cannot express, so every new crash has historically bought one more
   patch range (upstream #290, #359, #360 — see §4.4).
5. **Whack-a-mole is over for the crash class.** `cg_audit.py` re-derives the
   whole finding in ~40 s and is checked against source constants at run time.
6. **The simulation is not the problem.** Statcheck reported **zero state
   divergences** through the faulting frame — this is purely a rendering/CG
   defect. The engine stays CPS3-accurate right up to the fault.

---

## 3. Current status

| Item | Status |
|---|---|
| Denjin crash root cause | **CLOSED** — identified, ASan-reproduced (§5) |
| Full-cast crash-class audit | **CLOSED** — 66 cells, Elena only (§7.2) |
| Elena crash fix | **OPEN** — delta measured (−0x6F42), not implemented (§8.A) |
| Elena OVCT unpatched tail | **OPEN** — latent, currently unreachable (§8.B) |
| 1,694 wrong-sprite cells | **OPEN** — enumerated, not triaged individually (§8.C-E) |
| Shape-divergent scripts (316) | **OPEN** — enumerated, need manual review (§11.2) |
| Upstream issue #363 | **OPEN** upstream; our findings not yet reported (§13) |
| Any code change | **NONE MADE** — this was investigation only |

No repository files were modified during this work.

---

## 4. Background: how arcade balance works

### 4.1 The two data universes

3SX is a decompiled PS2 port. "Arcade balance" replaces the PS2 character data
with the original CPS3 arcade data parsed out of the ROM, so the game plays with
arcade balance/frame data instead of the PS2 revisions.

The two datasets describe the *same* animations but index sprites in **different
namespaces**. That mismatch is the entire subject of this document. Upstream's
own framing, from Artem in Discord (2026-08-22):

> "whenever the game wants to render a different sprite for a character/entity
> it requests it using its index. and indices don't fully line up between PS2 and
> CPS3. I don't know why they had to re-map indices though."

### 4.2 Boot pipeline (our fork)

1. `src/main.c:474` → `ArcadeBalance_Init()`.
2. `src/arcade/arcade_balance.c:136` → `ArcadeCharData_Init()`.
3. ROM resolved from three candidate paths, loaded by `Rom_Load`
   (`src/arcade/rom_load.c:479`), which content-matches four 2 MiB SIMM slices by
   CRC32 pre-filter + pinned SHA-256 (`rom_load.c:41-46`, `:161-215`) and
   XOR-decrypts them into one 8 MiB big-endian image (`rom_load.c:94-118`,
   `cps3_decrypt.c:33-37`).
4. For each of `NUM_CHARS` = 20 characters (`src/constants.h:36`), 25 sections
   are parsed at hard-coded ROM offsets from `location_data[]`
   (`arcade_char_data.c:843-1384`) into `CharDataImage.spans[]`.
5. `coalesce_adjacent_sections` (`arcade_char_data.c:263-314`) merges adjacent
   ROM runs "because CPS3 data sometimes indexes across named section
   boundaries" (`:543-544`).
6. `arcade_balance.c:43-83` reads the **PS2** char-data tail per character out
   of the AFS (via `texgrpdat[character+1].apfn` / `.to_chd`) and calls
   `ArcadeCharData_Apply3SXRenderingConventions` (`:73`).
   **All-or-nothing:** any single character failing drops the whole session to
   PS2 balance (`:76-79`, `:143-149`).
7. On success, `digest = ArcadeCharData_ComputeDigest()` (`:151-152`), carried in
   the MIST netplay handshake so peers with differing adapted data reject rather
   than desync.

### 4.3 Runtime install and render

- `src/sf33rd/Source/Game/rendering/texgroup.c:417-437`: when arcade balance is
  on, the arcade `CharInitData` **replaces the PS2 one wholesale** via
  `SDL_copyp(dst, arcade_data)` (`:437`). Note the **texture/trans tables stay
  PS2** (`:395-397`).
- `charid.c:87-96` fans it into the live `WORK`: `char_table[0..9]` ←
  nmca/dmca/btca/caca/cuca/atca/saca/exca/cbca/yuca, plus `overlap_char_tbl`←ovct,
  `olc_ix_table`←ovix, `hit_ix_table`←hiit, `att_ix_table`←atit.
- Per-frame, script execution produces `wk->cg_number`, which reaches the
  renderer as:

```c
n = wk->cg_number;                 /* u16, 0..65535, straight from char data */
i = obj_group_table[n];            /* (1) NO BOUNDS CHECK — table is [37664]  */
if (i == 0) return;                /*     gap -> clean skip                    */
if (texgrplds[i].ok == 0) return;  /*     group not loaded -> clean skip       */
n -= texgrpdat[i].num_of_1st;      /* (2) can go negative                      */
trsbas = (u16*)(texgrplds[i].trans_table + ((u32*)texgrplds[i].trans_table)[n]);
count = *trsbas;                   /* (4) DEREFERENCE of a wild address        */
```

`obj_group_table` is `const u8 [37664]`
(`src/sf33rd/Source/Game/rendering/chren3rd.c:8`, decl `chren3rd.h:6`).
The idiom above repeats at **nine sites** in our fork's
`src/sf33rd/Source/Game/rendering/mtrans.c`: `:174-187`, `:273-284`, `:362-379`
(`getObjectHeight`), `:415-427`, `:684-696`, `:807-819`, `:1088-1100`,
`:1216-1228`, `:1475-1487`. On upstream @ 513380f9 the faulting site is
`mtrans.c:400` in `mlt_obj_trans_ext`.

### 4.4 The remap — the single point of translation

**Only one value in the entire pipeline is ever translated: `cg_number`.**

`remap_cg_number()` — `src/arcade/arcade_char_data.c:85-109`, applied at `:188`
and nowhere else:

```c
if (value < 0x400) return value;             /* :86-88  low CGs pass through   */
delta = cg_maps[character].default_delta;    /* :90     one delta per character*/
for each range: if (first <= value <= last) { delta = range->delta; break; }
adjusted = value + delta;
if (adjusted < 0 || adjusted > UINT16_MAX) return value;   /* :104-107 CLAMP   */
return adjusted;
```

Three properties matter, and all three are load-bearing defects:

1. **The only guard is `UINT16_MAX`.** Nothing ties the result to
   `obj_group_table`'s 37,664 entries. 37664 = 0x9320, so any result in
   [0x9320, 0xFFFF] is an out-of-bounds read.
2. **Failure to remap is silent and returns the raw CPS3 value** (`:104-107`).
   No log, no adaptation failure. Remy hits this path (§7.4).
3. **One delta per character cannot express cross-bank references.** When a
   script points at *another* character's sprite bank, the character's own delta
   scatters it somewhere wrong (§7.3).

Applied to the 10 script sections only (nmca, dmca, btca, caca, cuca, atca,
saca, exca, cbca, yuca — `arcade_char_data.c:501-510`). Within each cell, only
`cg_number` is remapped; `cg_se`, `cg_olc_ix`, `cg_hit_ix`, `cg_att_ix`,
`cg_extdat..cg_eftype`, `cg_zoom`, `cg_next_ix`, `cg_status` are byte-passed.
The other 15 sections are installed **raw**.

### 4.5 The one other adaptation: OVCT

`ArcadeCharData_Apply3SXRenderingConventions` (`arcade_char_data.c:647-689`)
touches exactly **one** section, OVCT (overlap parts), and only three of its
fields:

- `:672-677` verification pass: every common element must match on
  `parts_hos_x/y, colmd, prio, flip, timer, disp, nix`
  (`overlap_behavior_matches`, `:611-616`). Any divergence fails the whole
  adaptation. **`parts_char` is deliberately NOT compared.**
- `:679-685` the entire remap: for `i < common_count`, three fields are
  overwritten from PS2 — `parts_colcd`, `parts_mts`, `parts_char`.
- `common_count = SDL_min(arcade_count, ps2_count)` (`:670`). **Anything past
  that keeps raw CPS3 values** (see §8.B).
- `:648, 654-656` one-shot latch per character: a second call returns `true`
  without inspecting anything.

### 4.6 Upstream fix history for this exact class

All of these are single-range patches added after a crash was reported:

| Commit | PR | What it added |
|---|---|---|
| `da493399` | #181 | Arcade char data (initial reader) |
| `ae309dc4` | #185 | Adjusted the remap cutoff `0x800` → `0x400` |
| `1a2d354e` | #196 | Fixed Ibuki's cg numbers |
| `1f64b621` | #283 | Missing-palette crash; introduced `Apply3SXRenderingConventions`, **deleted** the richer `remap_ovct_parts_char` |
| `3b38d29d` | #290 | **Fix Elena's CG range mapping** — added `0x9C88-0x9CC1 → -0x6F08` |
| `0ef92066` | #350 | Preserve arcade data adjacency (`coalesce_adjacent_sections`) |
| `fee66bf8` | #359 | **Fix X.C.O.P.Y. crash against Sean** — added `0x70F4-0x70FF → -0x2F74` |
| `bcd7b892` | #360 | Fix X.C.O.P.Y. transition animations — added **17** ranges at once |

**Read #290 and #359 carefully: they are the same bug as #363.** A single move
crashed because a flat `default_delta` mis-mapped one CG sub-range. Elena's
Denjin-shock cells sit *immediately above* the range #290 added.

### 4.7 Why nothing catches this automatically

`arcade_balance.c:114-119` pins the test runner to PS2 balance:

> "The frame-data suite's corpora encode PS2-balance expectations; the harness
> must resolve identically on every machine regardless of ROM presence."

So the 1,349-entry frame-data suite **never exercises the arcade path**. Also
note `SDL_assert` is compiled out in Release (`-DNDEBUG`), including the
`SDL_assert(adapted && arcade_data != NULL)` at `texgroup.c:424`.

---

## 5. Root cause: the Denjin crash (upstream #363)

### 5.1 The report

Upstream issue **#363**, "Ryu's Denjin Hadoken crashes the game", filed by Artem
(apstygo) 2026-08-19, milestone 1.0, still OPEN. Body in full:

> This looks like a CG mapping issue
> Replay ID: 1787095232817-6231.7 — Game index: 2

Attachment `game_2.scrd.zip` → `game_2.scrd` (23,052,544 bytes), downloadable
from `https://github.com/user-attachments/files/31238480/game_2.scrd.zip`.

### 5.2 The reproduction (verified 2026-08-29)

Built upstream @ 513380f9 with statcheck + ASan and ran the attached replay:

```
==96666==ERROR: AddressSanitizer: global-buffer-overflow
READ of size 1 at 0x000104dba0c2 thread T0
    #0 mlt_obj_trans_ext      mtrans.c:400
    #1 mlt_obj_trans          mtrans.c:638
    #2 Mtrans_use_trans_mode  aboutspr.c:290
    #3 sort_push_request      aboutspr.c:389
    #4 reqPlayerDraw          plcnt.c:479
    #5 Game2_1                game.c:523
0x000104dba0c2 is located 482 bytes after global variable 'obj_group_table'
  (0x000104db0bc0) of size 37664
```

From lldb at the fault: `wk->cg_number = 38146` (0x9502). Array max valid index
is 37,663 — **482 past the end, exactly matching ASan's offset**.
`wk == &plw[0]` and the caller is `plcnt.c:479` → **Player 1**.

**Match context:** `My_char[0] = 8` (Elena, P1), `My_char[1] = 2` (Ryu, P2),
`Super_Arts[1] = 2` (SA-III = Denjin Hadouken). Statcheck `frame_index = 3913`.

### 5.3 Where the bad value comes from

A watchpoint on `plw[0].wu.cg_number` with condition `== 38146` caught the write:

```
frame #0: setupCharTableData   charset.c:121   (dst[i] = src[i])
frame #1: check_cgd_patdat     charset.c:2404
frame #2: check_cm_extended_code charset.c:434
frame #3: Damage_12000         plpdm.c:496
frame #4: Player_damage        plpdm.c:201
frame #5: player_mv_4000       plmain.c:326
frame #6: move_P1_move_P2      plcnt.c:982
```

At that stop `plw[0].wu.now_koc = 1` → `char_table[1] = cdat->dmca`
(`charid.c:80`), and `char_init_data[10].dmca ==
ArcadeCharData_Get(CHAR_ELENA)->dmca` (identical pointers).

**So: the out-of-range CG is read out of Elena's arcade-ROM damage-reaction
table while she is being shocked by Ryu's Denjin projectile.** It fires **19
times** across the replay with three consecutive CGs — 38146 (×9), 38147 (×6),
38148 (×4) — i.e. one 3-frame shock animation.

### 5.4 Why the naive hypothesis was wrong (recorded so it isn't re-run)

The obvious theory — that Ryu's Denjin data is broken — was **disproven by
measurement**. Ryu's entire Denjin chain (`saca[40..42]`, `cbca[13..18]`) is
byte-identical between CPS3 and PS2 after remap; all 676 of his distinct CG
numbers land inside his own group 3 (`0x0A20..0x0DCA`, `texgrpdat[3].num_of_1st
= 2592`); his projectile rows (`tama_data` 8/9/10/11/52) match. His OVCT theory
also died: PS2 Ryu OVCT = 56 entries ≥ arcade's 54, so every `parts_char` is
patched.

**Do not re-investigate Ryu's tables. The victim's table is the carrier.**

### 5.5 The symptom is memory-layout-dependent (important)

The **non-ASan** build of the same commit **completes the replay with exit 0**.
From lldb on that binary: `obj_group_table[38146]` is `const`, so it lands in
`__TEXT.__const`; index 38146 reads a **zero byte inside the neighbouring
`color_file`**, so `mtrans.c:402` (`if (i == 0) return;`) silently drops the
sprite.

Consequences:

- On macOS the bug is a missing sprite, not a crash.
- On a target where that neighbouring byte is **non-zero**, `i` becomes an
  arbitrary value indexing `texgrplds[100]`, and the code then either spins
  forever in upstream's `while (1) {}` or dereferences a garbage `trans_table`.
- **On our fork**, the 2026-04-29 trap sweep converted those hangs into
  skip+log, so our worst case is a garbage-pointer deref → **SIGSEGV**. No
  SIGSEGV handler is installed (`src/main.c:176-191` registers only
  SIGINT/HUP/TERM/USR1/RTMIN+2..4), so on the MiSTer this appears as
  **`exit=139`** in `/media/fat/games/3s-arm/logs/last-run.log` with **no
  backtrace**. (`exit=134` would instead mean `fatal_error()`/`abort()`.)

**Therefore Artem's ~95.5% daily replay pass rate very likely undercounts this
class** — a non-ASan runner can pass a replay that is performing OOB reads.

### 5.6 Control run

Same binary, `arcade-balance = false`: statcheck stops at
`test_runner_compare.c:318: lvr_3sx->sw_new (6) != lvr_cps3->sw_new (2)`,
exit 1, at `frame_index = 720`. Frame 3913 is unreachable without arcade
balance — confirming the crash path is arcade-balance-only.

---

## 6. The audit: method

`cg_audit.py` (preserved, §9) decodes **every** arcade script cell for all 20
characters and pairs it against the PS2 counterpart.

- **Constants are parsed from source at run time, not hand-copied** — so the
  audit stays honest as the code changes. Verified header line:
  `obj_group_table=37664 effinitjptbl=59 decode_chcmd=125
  sound_effect_request=1024 tama_data=243 sa_sign_data=69`.
- Arcade side: `rom.bin`, produced by `decrypt.py` from `sfiii3nr1.zip`; the four
  SIMM SHA-256s were confirmed equal to the digests pinned at `rom_load.c:41-45`.
- PS2 side: `SF33RD.AFS`, entry `texgrpdat[char+1].apfn`, tail at `.to_chd`, then
  `get_ps2_section_span` logic mirrored from `arcade_char_data.c:618-645`.
- **133,901 cells audited.** Runtime ~40 s.

### 6.1 Violation classes

| Class | Meaning | Severity |
|---|---|---|
| **(a)** `a_ogt_oob` | remapped CG ≥ 37664 | **crash class** — OOB read |
| **(b)** gap | remapped CG hits an `obj_group_table` zero | silent sprite drop |
| **(c)** mismatch | in-range but ≠ the PS2 counterpart's CG | wrong sprite drawn |
| — | `c_own_group` / `c_other_group` | sub-split by whether it stays in the character's own texture group |
| manu | script shapes differ; no cell-aligned oracle | needs human review |

Two discriminators had to be built in to avoid false positives, and are worth
knowing if the script is ever modified:

1. `cg_se` bit `0x800` selects the random-SE table rather than indexing
   `sound_effect_request[]` (`charset.c:2723-2727`). Checking `>= 1024` naively
   produced **1,848 false positives**.
2. **Any value identical on both sides is a pre-existing property of shipping PS2
   data, not an adaptation defect.** This filter cleared, among others, Makoto's
   `atca[108] → jsr(cbca,38)` against a 30-entry `cbca` — PS2's Makoto `cbca` is
   *also* 30 entries and PS2's `atca[108]` issues the same `jsr`, so it is a
   latent hazard in the shipped PS2 build too, and out of scope here.

---

## 7. The audit: results

```
char    cells |  (a)  (b) (c)wg (c)og  manu | ovct a/p    ovix a/p
GILL     6634 |    0    0     0     0     8 | 392/396 ok  146/148 short
ALEX     6546 |    0    0     0     0    21 | 57/59   ok  53/55   short
RYU      5099 |    0    0     0     0    14 | 54/56   ok  7/9     short
YUN      8859 |    0    0     0     1    31 | 6/8     ok  20/22   short
DUDLEY   7051 |    0    0    23     0    16 | 178/180 ok  41/43   short
NECRO    6484 |    0    0    23     1    16 | 46/48   ok  10/12   short
HUGO     5952 |    0    0    23     1    15 | 71/73   ok  62/64   short
IBUKI    8222 |    0    0    23   408    21 | 2286/2296 ok 2230/2235 short
ELENA    7769 |   66    0    23     0    13 | 91/85 UNPATCHED-TAIL! 91/85
ORO      6191 |    0    0    23     0    14 | 67/69   ok  41/43   short
YANG     8613 |    0    0    23     0    30 | 20/22   ok  20/22   short
KEN      5056 |    0    0    26     0     9 | 42/44   ok  24/26   short
SEAN     5547 |    0    0    31     0     9 | 25/27   ok  25/27   short
URIEN    6268 |    0    0    23   256    21 | 329/352 ok  187/189 short
AKUMA    5881 |    0    0    23     6    15 | 115/117 ok  34/36   short
CHUNLI   6726 |    0    0     0    72     9 | 75/78   ok  75/77   short
MAKOTO   7192 |    0    0   521     0    15 | 252/254 ok  139/141 short
Q        7178 |    0    0    29     0    15 | 18/20   ok  18/20   short
TWELVE   7268 |    0    0    88     0    11 | 133/135 ok  133/135 short
REMY     5365 |    0    0    47     0    13 | 42/44   ok  31/33   short
TOTAL         |   66    0   949   745   316
cells audited: 133901
```

### 7.1 Validation — the audit rediscovers all three historical fixes

Counterfactual runs with each shipped fix reverted (`counterfactual.py`,
`cg_counterfactual.json`):

| Reverted | class (a) | class (c) | Where it reappears |
|---|---|---|---|
| baseline (HEAD) | 66 | 1694 | — |
| **#290** (Elena `0x9C88-0x9CC1`) | **304** | 1694 | Elena, as **class (a)** — the crash class |
| **#359** (Sean `0x70F4-0x70FF`) | 66 | **1740** | Sean `saca[0,1,6,7]`, as class (c) |
| **#360** (all `0x7070-0x714B`) | 66 | **2530** | `saca[0,1,6,7]` of **all 20** characters (882 violations) |
| all three | 304 | 2530 | — |

This is the evidence that the audit would have caught each historical crash
*before* a user hit it.

### 7.2 Class (a) — the crash class: Elena only, 66 cells, 3 sites

| Table | Raw CG | Remapped | PS2 counterpart | Cells | Scripts |
|---|---|---|---|---|---|
| `dmca` | 0x9D22-0x9D24 | 38146-38148 | 0x2DE0-0x2DE2 | 24 | 82-89 |
| `btca` | 0x9D22-0x9D24 | 38146-38148 | 0x2DE0-0x2DE2 | 4 | 15 |
| `exca` | 0x9CFC-0x9D21 | 38108-38145 | (shape differs) | 38 | 58-65 |

- `dmca[82..89]` is the electric-shock damage animation — **the #363 crash**.
- `btca[15]` (a knockdown) uses the **same three sprites** and was not in the
  bug report.
- `exca[58..65]` is a 38-cell contiguous run, also unreported.

All sit immediately above the #290 range's `last = 0x9CC1`, take the default
delta **−0x0820**, and land past `obj_group_table[37664]`.

**Measured correct delta for the adjacent block: −0x6F42** (from the 16
cell-aligned pairs; #290's range uses −0x6F08).

Confidence note: for class (a) the OOB is a property of the arcade value plus
the remap alone, so **all 66 are certain**. The high/low split in the JSON
(16 high, 50 low) refers only to whether a cell-aligned PS2 counterpart could be
annotated, not to whether the cell is OOB.

**Fixing only what the bug report describes would fix 24 of 66 cells.**

### 7.3 Class (c) — wrong sprites: 1,694 cells, three sub-families

**(i) Cross-bank references — 949 cells.** The clearest case: raw `0x0CB4`
appears in `yuca[68..75]` of **13 characters**. Each character's own delta
scatters it to a different wrong group (Dudley→grp 2, Hugo→grp 1, Urien→grp 1,
Ken→grp 12, Sean→grp 13, Akuma→grp 15…), while **PS2 resolves it with Ryu's
−0x1E0 to group 3**. Same shape: Makoto `atca`/`exca` (521 cells, PS2 wants
−0x1E0), Twelve `nmca`/`cuca`/`exca` (88, PS2 wants Necro's −0x600 — the
X.C.O.P.Y. family), Q `cuca[37]` (29), Sean `cuca[64]` (8), Ken `caca[18..20]` (3).

**This sub-family is unfixable by adding more per-character ranges** without
either (a) ranges fine enough to isolate every cross-bank reference, or (b) a
model change (§8.E).

**(ii) Off-by-N deltas inside the character's own group — 745 cells.**
- **Ibuki**: `saca[56..59]` + `yuca` — 408 cells where arcade delta −0x74D0 is
  **exactly one less** than PS2's −0x74CF. Draws the neighbouring sprite.
- **Urien**: `yuca[8..15]` — 256 cells spanning **ten distinct correct deltas**
  (−0xC6F … −0xC78) against the flat −0xC60.
- Smaller: Akuma `nmca[21,46]`, Yun `nmca[26]`, Necro `nmca[28]`, Hugo `btca[15]`.

**(iii) Probably-intentional 3SX content edits — not remap bugs.**
- **Chun-Li `saca[44..47]`, 72 cells**: PS2's CG is literally `0x0000` (blank).
  3SX appears to have removed those sprites deliberately. **Do not "fix" these
  without checking intent.**

### 7.4 The negative-clamp pass-through

Remy (and part of Makoto) hit `arcade_char_data.c:104-107`: `raw + delta < 0`
returns the value **unchanged**, silently passing e.g. Alex-range values
straight into Alex's group. Remy `nmca` (37+3 cells), `exca`, `cuca`. This is
the clamp behaving as written, producing wrong sprites rather than a fault.

### 7.5 OVCT / OVIX structural findings

- **Elena is the only character with `arcade_count > ps2_count`: 91 vs 85.**
  Parts **85-90 keep raw CPS3 `parts_char` 0x9CF6-0x9CFB (40182-40187)** — every
  one ≥ 37664. `read_ovct` (`arcade_char_data.c:370-393`) never remaps
  `parts_char`, and the patch loop only covers `i < common_count`.
  **Currently unreachable**: Elena's arcade cells emit `cg_olc_ix` 0-16 only, and
  no selected `ovix` entry reaches a part ≥ 85. A loaded landmine, not a live one.
- **Every other character's OVIX is 2-5 entries SHORTER than PS2's.**
  `wk->cg_olc = wk->olc_ix_table[wk->cg_olc_ix]` (`charset.c:2739`, `:2904`) is
  unbounded. Verified that no arcade cell currently emits an index past its own
  table, so this too is latent. (Confirmed PS2's own `dmca[82..89]` *does* use
  `olc = 112/128` → `cg_olc_ix` 7/8, where arcade uses 0.)

### 7.6 Over-declared `location_data` sizes (new finding, low priority)

Seven sections declare a size far past their real script data, so
`read_char_table`'s last script (`end_offset = location.size`,
`arcade_char_data.c:135`) decodes unrelated ROM as cells — and
`ArcadeCharData_ComputeDigest` **hashes that slack**, which matters because the
digest gates netplay compatibility.

```
HUGO   saca  declared=0x4164  real=0x34C4  slack=0x0CA0
ELENA  saca  declared=0x6638  real=0x6078  slack=0x05C0
ELENA  exca  declared=0x35BC  real=0x2E1C  slack=0x07A0
SEAN   caca  declared=0x12C0  real=0x0CD8  slack=0x05E8
URIEN  saca  declared=0x3A54  real=0x3184  slack=0x08D0
TWELVE saca  declared=0x5E50  real=0x5530  slack=0x0920
REMY   yuca  declared=0x73E0  real=0x11B0  slack=0x6230  (24 KB!)
```

Parsed-but-unreachable at run time (execution is bounded by each script's own
terminator), so not a live fault.

---

## 8. Worklist

Ordered by severity. **Nothing here has been implemented.**

### A. Elena's crash-class cells — 66 cells, 3 sites (DO FIRST)

The measured correct delta for the adjacent block is **−0x6F42**. The naive fix
(extend #290's range) is wrong: #290 uses −0x6F08, and the three sites span
`0x9CFC..0x9D24`, which is a *different* block from `0x9C88..0x9CC1`.

Suggested shape: add a new `CgRemapRange` to `elena_cg_ranges` covering
`0x9CFC-0x9D24` with delta **−0x6F42**, then re-run `cg_audit.py` and confirm
class (a) drops to **0**. Verify against the 16 cell-aligned PS2 counterparts
(`dmca[82,83,86,87]`, `btca[15]` → PS2 `0x2DE0-0x2DE2`) rather than trusting the
arithmetic alone. The `exca[58..65]` run has no cell-aligned oracle, so confirm
visually or via a replay that exercises it.

### B. Elena's unpatched OVCT tail — parts 85-90

Latent but real. Options: extend the patch loop past `common_count` with an
explicit remap for `parts_char`, or add a bounds check where `parts_char`
becomes `cg_number` (`eff01.c:169`). Note upstream **deleted** a richer
per-character OVCT remap (`remap_ovct_parts_char`) in #283 — its Ibuki/Urien
bands have no equivalent today.

### C. A bounds guard (cheap, high value, defensive)

Neither `remap_cg_number` nor any of the nine `mtrans.c` sites checks against
37,664. A guard turns every *future* instance of this class from a
layout-dependent SIGSEGV into a dropped sprite plus a log line — which is
exactly what the 2026-04-29 trap sweep did elsewhere in the tree. Consider both:
a clamp/reject in `remap_cg_number` (with a log) and a bounds check at the
`obj_group_table[n]` sites.

### D. The off-by-N deltas — Ibuki (408) and Urien (256)

Mechanical: the PS2 counterparts give the exact per-script deltas. Ibuki needs
−0x74CF for `saca[56..59]` and parts of `yuca`; Urien needs up to ten distinct
deltas for `yuca[8..15]`. Both are range-table work, no model change needed.

### E. The cross-bank cluster — 949 cells (needs a model decision)

The 13-character `yuca[68..75]` cluster and Makoto/Twelve/Q/Sean/Ken cannot be
expressed as "one delta per character" — the correct answer is *another
character's* delta. Two ways out:

1. **More ranges** — keep the current model, add narrow ranges per cross-bank
   block. Works, grows the table, stays whack-a-mole-shaped but is now
   *audit-driven* rather than crash-driven.
2. **Change the model** — allow a range to name a target group/bank rather than a
   raw delta, so a cross-bank reference is expressed as intent. Cleaner, larger
   change, and would need upstream buy-in to avoid divergence.

Recommend deciding this **with Artem** before writing code (§13).

### F. Chun-Li's 72 blank-CG cells — verify intent, probably no-op

Confirm whether 3SX intentionally blanked those sprites. If intentional, mark
them excluded in the audit so they stop appearing as findings.

### G. Over-declared section sizes (§7.6)

Tighten the seven declared sizes to real extents. Note this **changes
`ArcadeCharData_ComputeDigest`**, which is carried in the MIST handshake — so it
is a peer-compatibility-breaking change and must ship on both sides together.

---

## 9. Tooling (in-repo and verified working)

**Location:** `tools/arcade-audit/` on branch `fix/arcade-cg-mapping`. See the
README there for the short version.

Re-verified after the move: `python3 tools/arcade-audit/cg_audit.py` reproduces
`TOTAL | 66 0 949 745 316` and `cells audited: 133901` identically.

| File | Purpose |
|---|---|
| `cg_audit.py` | **The main deliverable.** Full 20-character audit. Parses constants from repo source at run time. Writes `cg_audit.json`. ~40 s. |
| `counterfactual.py` | Re-runs the audit with historical fixes reverted → `cg_counterfactual.json` |
| `decrypt.py` | Rebuilds `rom.bin` from `sfiii3nr1.zip` (prints SIMM SHA-256s for verification) |
| `afs.py` | AFS container parser (magic `AFS\0`, 1535 entries) |
| `parse.py`, `scan.py`, `cgscan.py` | Arcade-side script decoders / early sweeps |
| `ps2scan.py`, `cmpovct.py`, `fulldiff.py` | PS2-side decode, OVCT compare, full arcade-vs-PS2 script diff |
| `rom.bin` | Decrypted 8 MiB CPS3 image (sha256 starts `c15743e350011f6a…`). **Gitignored** — rebuild with `decrypt.py` |
| `cg_audit.json` | Every violation, machine-readable (493 KB) |
| `audit_summary.txt`, `audit_run.txt` | Human-readable audit output |
| `denjin_oobcount.log` | The 19 OOB hits from the ASan run |

**Paths resolve automatically.** The repo root is derived from the script's own
location, so the audit reads the source tree of whichever worktree it sits in —
important, since the whole point is auditing *this* branch's tables. Override
with env vars when needed:

| Var | Default |
|---|---|
| `ARCADE_AUDIT_REPO` | repo root, derived from `tools/arcade-audit/../..` |
| `ARCADE_AUDIT_AFS` | `~/Library/Application Support/CrowdedStreet/3S-ARM/resources/SF33RD.AFS` |
| `ARCADE_AUDIT_ROM` | `rom.bin` beside the scripts |
| `ARCADE_AUDIT_ROMZIP` | `~/Library/Application Support/CrowdedStreet/3S-ARM/resources/sfiii3nr1.zip` |

**Asset locations (verified):**
- PS2 data: `~/Library/Application Support/CrowdedStreet/3S-ARM/resources/SF33RD.AFS`
  — 642,492,416 bytes, md5 `cc788f2ba398c7e464736f4b6d00bc82`.
  **Note the `3SX/` copy is a DANGLING SYMLINK** to a non-existent
  `/Users/sb/Developer/3sx-ios/SF33RD.AFS` — do not use it.
- CPS3 ROM: same `resources/` dir as `sfiii3nr1.zip`; also
  `/Users/sb/Developer/fbneo-replay-runner/roms/sfiii3nr1.zip` (what `decrypt.py`
  points at).
- Crash replay: re-downloadable from the issue-#363 attachment URL (§5.1).

Also in-repo and related: `tools/compare_char_data.py` — upstream's own
"compare CharInitData between the arcade version and the port" tool.

---

## 10. Reproducing the crash (exact recipe)

Two corrections to assumptions that cost time the first round:

- `THREESX_STATCHECK` is a **cmake option, not a build type**
  (`CMakeLists.txt:22`, `docs/building.md:57-70`).
- `CMAKE_BUILD_TYPE=Debug` **does not build** against the local prebuilt
  GekkoNet — Debug defines `NETPLAY_ENABLED` and fails with
  `error: use of undeclared identifier 'GekkoReplayFinished'`. Use
  `RelWithDebInfo`.

```bash
# 1. upstream source without network
git -C /Users/sb/Developer/3sx-mister archive upstream/main | tar -x -C <workdir>/src-upstream
ln -sfn /Users/sb/Developer/3sx-mister/third_party <workdir>/src-upstream/third_party

# 2. build statcheck + ASan
CC=clang CXX=clang++ cmake -S src-upstream -B src-upstream/build-statcheck-asan \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTHREESX_STATCHECK=ON \
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O1 -g -DNDEBUG -fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g -DNDEBUG -fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build src-upstream/build-statcheck-asan --parallel 10

# 3. run (pref dir ISOLATED — see below)
CFFIXED_USER_HOME=<workdir>/fakehome \
  src-upstream/build-statcheck-asan/3SX.app/Contents/MacOS/3SX \
  --ram-archive <path>/game_2.scrd --headless
```

**Pref-dir isolation matters.** `Paths_GetPrefPath()` is
`SDL_GetPrefPath("CrowdedStreet","3SX")` (`src/port/paths.c:5-16`). `HOME=` does
**not** move it; **`CFFIXED_USER_HOME=` does**. Put a config containing
`arcade-balance = true` in
`<fakehome>/Library/Application Support/CrowdedStreet/3SX/config`, and symlink
`resources/SF33RD.AFS` + `resources/sfiii3nr1.zip` into that tree. This keeps the
real `~/Library/Application Support/CrowdedStreet/3SX/` untouched.

Invocation contract from `tools/statcheck_runner.py:157`:
`[exe, "--ram-archive", <game_N.scrd>, "--headless"]`. **There is no game-index
argument** — `game_2.scrd` *is* the single game; `ReplayGame_Init`
(`src/test/replay_game.c:26-69`) finds the game-start frame itself.

To prove arcade balance was actually on, breakpoint `ArcadeCharData_Init` and
read `target variable is_enabled` (upstream: the static at
`arcade_balance.c:8`, set from `Config_GetBool(CFG_ARCADE_BALANCE)` at `:12`).

---

## 11. Blind spots — what static auditing cannot find

### 11.1 The three tiers

1. **Crash class (a)** — fully statically discoverable from the arcade ROM +
   `cg_maps[]` alone. **Closed** by `cg_audit.py`.
2. **Wrong-sprite class (c)** — statically discoverable *given the PS2 AFS as
   oracle*, wherever scripts are cell-aligned. **Enumerated** (1,694).
3. **State/shape divergence** — **not** statically discoverable. No table-bounds
   oracle exists.

### 11.2 The tier-3 residue

- **316 shape-mismatched scripts** (arcade vs PS2 cell counts differ) have no
  automatic verdict. They are enumerated in `cg_audit.json` (`manu` column) and
  are exactly where tier-3 bugs live.
- **A concrete example found on the Denjin path itself:** `cbca[19..23]` — the
  five scripts `uja7` lands on after the projectile is released — differ:

  | | arcade | PS2 |
  |---|---|---|
  | cell 0 | `ctr=16, cancel=0, cg_effect=0` | `ctr=4, cancel=64, cg_effect=31, cg_eftype=1` |
  | cell 1 | `back` | `ctr=12, cg_effect=0` |
  | cell 2 | — | `back` |

  PS2 splits the 16-frame release freeze into 4+12 and fires
  `effinitjptbl[31] = setup_meoshi_hit_flag`; the arcade script fires no effect.
  Both `cg_effect` values are in range, so this is a **behavioural** divergence,
  not an OOB. Unresolved whether it is a port bug or a real CPS3-vs-PS2
  difference.
- **Two Denjin-unique engine mechanisms** that no data sweep can validate:
  `Att_DENJINHADOUKEN` calls `char_move()` up to **5 extra times per tick**
  (`plpat02.c:40-49` — no other move does this), and it is the only Ryu move
  using the `cmj7` reserved-jump slot (`comm_rja7`/`comm_uja7`,
  `charset.c:943-955`), which is consumed with no validation of the stored
  koc/ix/pat.
- **Hard-coded C constants that mirror data layout** are a related hazard and are
  grep-able: `now_koc == 8 && char_index == 13` (`plpat02.c:37`,
  `com_sub.c:300`) hard-codes Denjin's `cbca` slot in C.

### 11.3 The systematic net for tier 3

Run the ASan statcheck build over a **large replay corpus** (`tools/fcade-replays`
bulk download → `fbneo-replay-runner` → SCRD → ASan statcheck). Per-frame RAM
equality is the oracle; ASan removes the layout-luck masking documented in §5.5.

**A coverage counter would convert unknown-unknowns into a number:** the audit
already enumerates all 133,901 cells, so counting executed (character, table,
script, cell) tuples at the `setupCharTableData`/`char_move` choke point and
diffing against that universe yields exactly which cells have never been
exercised. That set *is* the residual risk, and it also tells you which
characters/moves to go fetch replays for.

---

## 12. Not verified (stated so nothing is mistaken for a finding)

- **Elena's pre-remap raw values for the class-(a) cells were not read directly**
  from the running process — the variable is optimized out at
  `arcade_char_data.c:87`. The raw values come from the ROM parse and the delta
  from arithmetic (`0x9502 = 0x9D22 − 0x820`). The OOB itself *was* observed.
- **Whether the 66 Elena cells all crash on the MiSTer.** Only the `dmca` shock
  path was observed faulting, on macOS, under ASan. `btca[15]` and `exca[58..65]`
  are OOB by measurement but were not executed in a run.
- **No on-device (MiSTer/ARM) reproduction was attempted.** All runtime evidence
  is macOS + ASan on upstream @ 513380f9.
- **Whether the audit's 1,694 class-(c) findings are all real defects.** Chun-Li's
  72 look intentional; the rest were not individually eyeballed.
- **Whether other characters have unreported class-(a) equivalents in data the
  audit cannot align.** The audit says no OOB is *producible*, which is stronger
  than "none observed" — but it rests on the parse being complete.
- **The `cbca[19..23]` divergence** (§11.2) — port bug or genuine version
  difference: unresolved.
- **Our fork's behaviour under the OOB** was reasoned from the trap sweep and the
  absent SIGSEGV handler, not observed. No `exit=139` was captured on device.

---

## 13. Upstream coordination

- Issue **#363** is open, milestone 1.0, assigned to nobody, zero comments.
- Artem said in Discord (2026-08-22): *"I don't have the tools to properly fix
  denjin right now so I'm taking some time to better understand the engine."*
  **The tooling in §9 is precisely those tools** — it decodes and diffs both
  datasets and enumerates every violation in the cast.
- Our fork carries the **identical defect** (same `remap_cg_number`, same Elena
  ranges) on `upstream-engine-fixes`. Any fix should be shaped so it can go
  upstream rather than diverge — especially the §8.E model decision.
- Worth telling upstream regardless of who fixes it: the bug is in the **victim's
  `dmca`**, not Ryu's data; there are **three** affected sites, not one; and the
  crash is **layout-dependent**, so a non-ASan replay runner can pass a replay
  that is reading out of bounds (§5.5).

---

## 14. Suggested next steps

1. **Fix Elena's 66 cells** (§8.A) — one range, delta −0x6F42, then re-run
   `cg_audit.py` and require class (a) = 0.
2. **Add the bounds guard** (§8.C) — converts the whole future class from
   layout-dependent crashes into logged sprite drops.
3. **Report findings on #363** (§13) — cheap, and prevents duplicate work
   upstream.
4. **Decide the model question** (§8.E) with Artem before touching the 949
   cross-bank cells.
5. **Land Ibuki + Urien** (§8.D) — mechanical, 664 cells, visible quality win.
6. **Wire the coverage counter** (§11.3) — turns the tier-3 blind spot into a
   measured number instead of an unknown.
