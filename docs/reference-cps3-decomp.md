# The `cps3-sf3iii` reference tree — what it is, and what it is worth

An external, MIT-licensed source reconstruction of the **arcade CPS3 program**
for the same game and the same ROM we target. It is a **second reader, never an
oracle.** This file records what it is, the one cross-check we ran against it,
and the four caveats that bound what any future cross-check can conclude.

Read this before citing it for anything.

## What it is

| | |
|---|---|
| Path | `/Users/sb/Developer/cps3-sf3iii` (snapshot, **not a git checkout**; files dated 2026-09-01) |
| ROM | `sfiii3nr1`, Japan 990512, NO CD — **the same set we decrypt to `tools/arcade-audit/rom.bin`** |
| Size | **787,084** lines of C, 11,367 files, 32 headers *(re-measured 2026-09-12; the "~109k" this row used to carry was a `find … \| xargs wc -l \| tail` artifact — `xargs` runs `wc` once per batch and only the LAST batch's `total` survives the tail, which on this tree is 108,929. Re-derive with `find … -print0 \| xargs -0 cat \| wc -l`.)* |
| Licence | MIT, "SFIII3 CPS3 decompilation contributors" |
| Build | Windows-only PowerShell + a user-supplied Hitachi SHC r26. WSL and Linux explicitly unsupported. **Not runnable on this machine** |

Because it is not a git checkout there is no commit to stamp a citation against.
**Cite file + symbol and note the snapshot date.** Never cite a line number.

Two tiers, and the difference decides how much a citation is worth:

- **`src/ghidra/`** — raw Ghidra output, ~11.2k files. 2,947 named `FUN_<addr>.c`;
  **8,613** carry an `@ 06xxxxxx` header *(re-measured 2026-09-12; the 8,705 this
  line used to carry does not reproduce. `find src/ghidra -name '*.c' -exec grep
  -lE '@ 06[0-9a-fA-F]{6}' {} + | wc -l` → 8,613. For the record, the alternate
  `@ 0x06xxxxxx` header style is a further 1,079 files, and neither count nor
  their union — 9,598 — is 8,705.)* **Addresses are rigorous here.** This is the
  tier worth citing for behaviour.
- **`src/recovered/code/`** — reviewed, 95 files. `scripts/verify_build.py`'s
  `recovered_source_boundary` check *fails the build* if `undefined1/2/4/8`,
  `param_N` or `local_xxxx` survive, so "clean" is machine-enforced rather than a
  convention. But function addresses are often dropped on promotion; data tables
  keep `/* CPS3 @0x... */` (660 of them).

## THE FOUR CAVEATS — all four bound every conclusion

1. **It does not reproduce the stock ROM.** `config/rom-hashes.json`'s
   `program_image_sha256` (the real decrypted program) and
   `config/reference-hashes.json`'s `linked_16m_image_sha256` (their build output)
   are **different values**. Their hash gate — and it is a real one, 14,409 link
   inputs each with a `reference_object_sha256` — proves **build determinism, not
   ROM equivalence.**

2. **It is only PARTLY independent of us.** Their README credits
   CrowdedStreet / apstygo (`crowded-street/3sx`, `crowded-street/3s-decomp`) and
   states that *"a significant portion of the recovered structure and naming was
   informed by the PS2 Anniversary Edition debug build"* — the same lineage as our
   tree. So:

   > **Their function NAMES and STRUCTURE are correlated evidence; agreement there
   > proves nothing. Only ROM-derived fact — addresses, instruction-level
   > behaviour, and data-table VALUES — is independent.**

   A cross-check concluding "they call it the same thing, so we are right" is
   worthless. Their naming is demonstrably unreliable in places: `0x020113B4` is
   `Play_Type` in one header and `Present_Mode` in another; `0x02016B3A` is
   `CHAR_MOTION_STATE` where we have `Bonus_Game_Flag`.

3. **Their per-character `DIVERGENT` table annotations are WRONG.** They declare
   `[20]` where the ROM has **21** entries and silently drop the arcade index-20
   row (Remy); what they annotate as "N/M entries differ, arcade kept" is the
   **index shift at 15** (the inserted Shin Gouki slot) counted as changed values.
   Ten were verified wrong against the ROM; 28 more are unverified. **Do not
   inherit them as divergence evidence.** For the 20 shared characters those tables
   are value-identical arcade-vs-PS2 — nothing for us to encode, and we encode none.

4. **AI disclosure and self-assessment.** Their README says AI tools were used for
   extraction, transformation and codegen, and to *"treat the rough source with
   suspicion."* Take them at their word.

## The cross-check we ran (2026-09-12)

Target: the four gates `docs/research-arcade-balance-desyncs.md` marks as resting
on **disassembly alone** — no runtime oracle covers them, so our reading of SH-2
was the only evidence. Every fact was re-read from the ROM with
`tools/cps3-disasm/cps3.py`, not taken from either tree's prose.

**All four CONFIRMED, each on ROM-derived (independent) evidence.**

- **E9a** `Win_13000` masks bit 12 — `table 0x061A38C0 [13] = 0x060C4D22`; mask
  `0x1000` loaded at `0x060C4DF4`; held arm returns without the `random_16` draw.
  `winner_type_tbl` decodes to 21 entries with `13` at arcade index 16, and indices
  14/15 both holding `8` — ROM-level support for the roster insertion.
- **E9b** the loser's clamp pair runs first — all six `lose_pl.c` counterparts
  dumped; nothing precedes the first `jsr 0x0611DFB8` but the register prologue and
  argument setup. Both `twelve_win_backjump` cases likewise.
- **E9c** `meta_win_pause` has one pair and no bonus-flag branch — and their reading
  is **better than ours**: the whole 218-byte routine contains **exactly two
  conditional constructs**, which makes the conclusion independent of what
  `0x02016B3A` is *named*. Our original proof relied on that literal's absence.
- **E1a** damage scale indexed by `Round_Level` unconditionally — both arms of
  `0x0609E36C`/`0x0609E3FA` add the value loaded from `0x0201137A`; zero referrers
  to `Play_Type` (`0x020113B4`) in range.

Nine of ten probe addresses resolved by plain grep, which is why this cost so
little: **their tree is addressable by our addresses.**

## What it does NOT cover

Checked specifically, because these are our open items: **no `get_new_parts_data`,
no `parts_nix`, no OVCT walk anywhere in either tier.** `olc_ix_table` appears only
as a struct field in `src/ghidra/work_init_zero.c`. Nothing there bears on Gill's
`+1`, Elena's OVCT tail, Dudley's dangling next-index, or the `olc_ix_table`
overruns. Their `src/recovered/code/eff01.c` holds only `parts_colcd_table`.

Their `src/recovered/data/rom_assets/` recipes (226) are all `DAT_060xxxxxx` —
**program-image** data, not graphics. They contain no CPS3→PS2 format conversion,
because the arcade program never needed one.

## How to use it

- **Do:** resolve one of our addresses to `src/ghidra/FUN_<addr>.c` and read the
  instruction stream as a second opinion. Compare data-table *values*.
- **Do:** study `scripts/bootstrap_rom.py`, `scripts/internal/cps3_split_simm.py`
  and `config/rom-hashes.json` as a model for hash-verified,
  read-from-the-user's-ROM asset provenance (`rom/.gitignore` keeps ROM data out of
  the tree entirely).
- **Do not:** copy code. Our architecture is a PS2 decompilation with arcade
  behaviour gated behind `ArcadeBalance_IsEnabled()`; theirs is the arcade program.
  Even where the licence allows it, the shapes do not match.
- **Do not:** treat agreement on a name as evidence, or a disagreement as a verdict
  against us. Where we and they differ, the ROM decides — and on the one place they
  materially disagreed with our docs (the roster-length tables), the ROM sided with
  us and their tree carries a truncation bug.
