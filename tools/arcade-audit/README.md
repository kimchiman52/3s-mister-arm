# Arcade (CPS3) char-data audit

Static audit of the arcade-vs-PS2 character-data mapping. Decodes every arcade
script cell for all 20 characters out of the decrypted CPS3 ROM, pairs each
against its PS2 counterpart from `SF33RD.AFS`, and reports every cell whose
remapped CG number is out of bounds, lands in a table gap, or disagrees with
PS2.

This is the tooling behind the root cause of upstream issue #363 ("Ryu's Denjin
Hadoken crashes the game"). **Full findings, worklist and repro recipe:**
`~/Desktop/3sx-arcade-rom-data-accuracy-2026-08-29.md`.

## Run it

```sh
python3 tools/arcade-audit/cg_audit.py        # ~40 s -> cg_audit.json + table
python3 tools/arcade-audit/counterfactual.py  # re-runs with historical fixes reverted
```

Expected at the time of writing (`fix/arcade-cg-mapping` branch point):

```
TOTAL | (a) 66  (b) 0  (c)wg 949  (c)og 745  manu 316
cells audited: 133901
```

`(a)` is the crash class — remapped CG >= 37664, past the end of
`obj_group_table` (`src/sf33rd/Source/Game/rendering/chren3rd.c:8`). All 66 are
Elena's; see the doc, §8.A. **A fix for that item should drive `(a)` to 0.**

## Inputs

`rom.bin` is **gitignored** (8 MiB decrypted ROM derivative). Rebuild it with:

```sh
python3 tools/arcade-audit/decrypt.py   # prints SIMM sha256s; compare to rom_load.c:41-45
```

Paths resolve automatically — the repo root is derived from this directory, so
it works from any worktree. Override with env vars if needed:

| Var | Default |
|---|---|
| `ARCADE_AUDIT_REPO` | repo root (derived from this file's location) |
| `ARCADE_AUDIT_AFS`  | `~/Library/Application Support/CrowdedStreet/3S-ARM/resources/SF33RD.AFS` |
| `ARCADE_AUDIT_ROM`  | `rom.bin` beside these scripts |
| `ARCADE_AUDIT_ROMZIP` | `~/Library/Application Support/CrowdedStreet/3S-ARM/resources/sfiii3nr1.zip` |

Note the `3SX/` (not `3S-ARM/`) copy of `SF33RD.AFS` is a dangling symlink — do
not point at it.

## Constants are parsed from source, not hand-copied

`cg_audit.py` reads `location_data[]`, `cg_maps[]` and `remap_cg_number` out of
`src/arcade/arcade_char_data.c`, `texgrpdat[]` out of `texgroup.c`,
`obj_group_table` out of `chren3rd.c`, and the effect/sound/command table sizes
out of their own sources at run time. Editing those tables changes the audit's
answer automatically — which is the point.

## Files

| File | Purpose |
|---|---|
| `cg_audit.py` | the audit; writes `cg_audit.json` |
| `counterfactual.py` | reverts #290/#359/#360 in-memory to prove the audit catches them |
| `decrypt.py` | rebuilds `rom.bin` from the ROM zip |
| `afs.py` | AFS container parser |
| `parse.py`, `scan.py`, `cgscan.py` | arcade-side script decoders / sweeps |
| `ps2scan.py`, `cmpovct.py`, `fulldiff.py` | PS2-side decode, OVCT compare, full script diff |
| `cg_audit.json`, `cg_counterfactual.json` | machine-readable results |
| `audit_summary.txt`, `audit_run.txt` | human-readable results |
| `denjin_oobcount.log` | the 19 OOB hits from the ASan repro |
