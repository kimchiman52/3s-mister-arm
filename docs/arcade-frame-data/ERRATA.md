# Oracle Errata

## Purpose

The JSON files in this directory (`akuma.json`, `oro.json`, `ibuki.json`,
`sean.json`, etc.) are md5-pinned upstream snapshots fetched from
`Coccis77/thirdstrikedatabot` (see each `tools/frame-data/corpus-*.yaml`
file header for the fetch URL and hash). **They are never hand-edited.**
When a row in one of these files cannot be meaningfully compared against
this repo's own live-measured S/A/R/adv/kd metrics — because the harness's
measurement model is structurally unable to represent what the field
describes, or because outside sources disagree with each other about the
correct arcade figure — that finding is recorded here instead of silently
patched into the oracle file or glossed over in a corpus comment.

This document is the errata register for that bucket. It does not change
any oracle JSON, any `corpus-*.yaml`, any `golden/*.tsv`, or any engine/
harness code. It is a read of what is already committed, cross-checked
against independently-maintained external frame-data sources.

**MASK-1 cross-ref (dated 2026-07-11):** the sentence above describes
this document's own ERRATA-1 step (doc-only, no corpus edits) — it is
not a permanent constraint on the corpus files. Since MASK-1
(`docs/frame-data-synthesis.md` §13.15, user decision 2026-07-11),
corpus entries with a STRUCTURAL verdict here now cite this register
directly via a `FIELD-MASKED (ERRATA)` comment and drop the assert on
the incomparable field only (never a value swap, never a new assert).
See §13.15 for the normative mechanism and the per-entry member table.

## Verdict vocabulary

- **STRUCTURAL** — the harness's own measurement model (the S/A/R
  decomposition, always non-negative, anchored on an active-hitbox
  window) cannot represent what the oracle field encodes for this move.
  This is not a disagreement about the correct number; independent
  external sources corroborate the oracle's published figure.
- **ORACLE-LIKELY-WRONG** — external sources independently agree with
  each other and disagree with the oracle; the oracle figure is probably
  a transcription or data-entry error. (No member of this bucket
  currently carries this verdict — see Sean below for the closest case,
  which does not qualify because the external sources themselves split.)
- **ORACLE-CONFIRMED** — the live measurement was suspected wrong, but
  external corroboration shows the oracle field is correct as published.
  (Not applicable to any row below; all four rows here are genuine
  incomparability/ambiguity cases, not simple measurement errors.)
- **ORACLE-WRONG-CONFIRMED** — a primary source (not just another
  secondary compilation) directly measured the move and confirms the
  oracle figure is wrong, not merely disputed. This is a stronger claim
  than `ORACLE-LIKELY-WRONG`: it moves the value from "probably a
  transcription error" to "objectively contradicted by real arcade
  hardware." (One member currently carries this verdict — Sean Dragon
  Smash Strong, upgraded from `UNVERIFIABLE` on 2026-07-11 by the arcade
  capture in `docs/arcade-frame-data/CAPTURE.md`; see §4 below.)
- **UNVERIFIABLE** — external sources conflict with each other, not just
  with the oracle or the live measurement, and no further primary source
  is available in this repo to break the tie. The open question stays
  open; no side is picked.

## Scope

The repo's own cross-cutting classification register for this bucket is
`docs/frame-data-synthesis.md:772-774` ("Oracle-side (not engine
classes)"):

> "Oracle-side (not engine classes): `ibuki-kubiori` two-phase
> incomparability, Sean Dragon Smash Strong suspected oracle duplicate
> row, Oro Oniyama negative-oracle-R incomparability."

That register names three items — Ibuki, Sean (flagged only as
"suspected"), and Oro — but the register quote above is not itself the
closed bucket this doc covers. Separately, grepping
`tools/frame-data/corpus-*.yaml` for a closed, orchestrator-GRANTED
`oracle-field-incomparable` xfail disposition turns up exactly **7
corpus entries** across three members (Akuma Ashura Senkuu ×1, Oro
Oniyama Jab ×3, Ibuki Kubiori ×3) — Akuma is part of this closed bucket
despite not being named in the register quote above; Sean is not. Sean
Dragon Smash Strong (×3 entries) has dual status: named in the register
above (as merely "suspected"), but tracked in the corpus as an **open,
unresolved "user review pending" item**, not a closed incomparability
disposition — it is included below because it was explicitly flagged for
external-source review, with its different, still-open status called
out.

**Scope amendment (dated 2026-07-11, MASK-1):** the "7 corpus entries"
figure above is the ERRATA-1-era bucket size, unchanged as a historical
count. The **MASK-1 masking bucket is a different count: 8 entries** —
the closed 7-entry bucket MINUS `ibuki-kubiori-lp-whiff`/`-block` (item
18(b), a value divergence on a comparable field, never masked) PLUS the
new §5 Sean Roll ×3 (STRUCTURAL, same zero-active class as §1). See
`docs/frame-data-synthesis.md` §13.15 for the normative masking
register and full member table; this document supplies only the
per-move ERRATA verdicts §13.15 cites.

---

## 1. Akuma — Ashura Senkuu (teleport), WHIFF

**Verdict: STRUCTURAL**

**Corpus citation:** `tools/frame-data/corpus-akuma.yaml:781,786` (entry
`akuma-ashura-whiff`), section comment `tools/frame-data/corpus-akuma.yaml:747-779`
(review ruling dated 2026-07-08 cited at lines 752 and 776):

```
expect: { R: from-qjson, xfail: "R incomparable - zero-active teleport, whole
duration classifies as saturated startup (measured S=T=72, A=0, R=0); overlay
R counts post-active ticks and there is no active anchor. Measurement-semantics
gap (review ruling 2026-07-08), NOT a timing divergence - see section comment." }
```

**Oracle row:** `docs/arcade-frame-data/akuma.json`, `"Ashura Senkuu"`:
`Startup: "-"`, `Hit: "-"`, `Recovery: "9"`, `Block_advantage: "-"`,
`Hit_advantage: "-"` — every field non-numeric except Recovery=9.

**Measured:** S=T=72 (meter-saturated), A=0, R=0 (the teleport's true raw
duration is ~75 frames, `atk_idle_F - move_start_F`; it produces no
hitbox at any point, so the harness's active-window anchor never fires).

**External sources (retrieved 2026-07-10):**
- EventHubs
  (`https://www.eventhubs.com/guides/2011/aug/21/akumas-frame-data-street-fighter-3-third-strike/`):
  `"Teleport | - | - | - | - | 20 | 45/29 | 9 | -"` — Startup 20, Active
  "45/29" (two travel-distance variants), **Recovery 9**.
- FAT-3S (`github.com/ThomasStvd/FAT-3S`,
  `src/js/constants/framedata/3SFrameData.json`, `Akuma.moves.normal`):
  `"Ashura Senkuu (Long)"`: `startup: 0, active: 66, recovery: 9`;
  `"Ashura Senkuu (Short)"`: `startup: 0, active: 50, recovery: 9`. Both
  variants: **Recovery = 9**, matching `akuma.json` exactly. FAT-3S's own
  convention folds the entire hitbox-less travel window into "active"
  (startup=0) rather than "startup" — i.e. even this independently
  maintained dataset needed its own special-cased decomposition to
  describe a hitbox-less teleport at all.

**Reasoning:** two independent external sources both confirm Recovery=9
as the correct published arcade figure — the oracle field itself is not
in dispute. The incomparability is entirely in this harness's own S/A/R
decomposition, which has no way to represent a move with zero measurable
active frames (no hitbox transition to anchor an active window), exactly
as the corpus's "measurement-semantics gap" framing states. Not a value
dispute.

**MASK-1 (2026-07-11):** the corpus entry `akuma-ashura-whiff` now
FIELD-MASKS the incomparable R field (drops the `expect.R` assert,
keeps outcome=WHIFF asserted) citing this section; see synthesis
§13.15.

---

## 2. Oro — Oniyama (Jab), WHIFF/BLOCK/HIT

**Verdict: STRUCTURAL** (with a caveat — see below)

**Corpus citation:** section comment `tools/frame-data/corpus-oro.yaml:729-754`;
entries `tools/frame-data/corpus-oro.yaml:766,781` (`oro-oniyama-lp-whiff`),
`:793,811` (`-block`), `:823,835` (`-hit`) — all three assert the same
measured **R=28, identical across WHIFF/BLOCK/HIT** (the xfail strings
themselves are not byte-identical: the block/hit entries add their own
adv/kd notes on top of the shared R=28 reasoning):

```
expect: { S: from-qjson, A: from-qjson, R: from-qjson, xfail: "oracle-field-
incomparable, not a divergence (see section comment): oro.json's
Recovery=-29 for this move is negative and cannot equal this harness's R
metric (always >=0); measured R=28 is a clean, self-consistent,
non-degenerate reading (S+A+R=44=T exactly, cut=1). S/A exact." }
```

**Oracle rows:** `docs/arcade-frame-data/oro.json`:

| Strength | Startup | Hit | Recovery | Block_advantage |
| --- | --- | --- | --- | --- |
| Jab | 6 | 10 | **-29** | -34 |
| Strong | 6 | 17 | **-30** | -42 |
| Fierce | 7 | 23 | **-33** | -50 |
| EX | 5 | 25 | **-43** | -60 |

**Measured (Jab only, tested):** S=6, A=10, R=28 identically on
WHIFF/BLOCK/HIT (S+A+R=44=T exactly, cut=1 on all three).

**External sources (retrieved 2026-07-10):**
- EventHubs
  (`https://www.eventhubs.com/guides/2011/aug/21/oros-frame-data-street-fighter-3-third-strike/`):
  `"Uppercut Light Punch (Oni Yama) | HL | 140(80(40)) | 15(3) | Su | 6 |
  9 | 29 | -22"` and `"Uppercut Medium Punch (Oni Yama) | HL | 160(100(40))
  | 15(3) | Su | 6 | 16 | 30 | -30"` — Recovery shown as **positive** 29
  (LP) / 30 (MP), contradicting `oro.json`'s negative sign. Active=9/16
  matches `oro.json`'s Hit field minus 1 exactly (10-1=9, 17-1=16) — a
  systematic 1-frame Active-vs-Hit counting-convention offset this source
  shows consistently elsewhere too.
- FAT-3S (`Oro.moves.normal`): `"LP Oniyama"`: `startup: 7, active: 10,
  recovery: -29, onBlock: -34`; `"MP Oniyama"`: `startup: 7, active: 17,
  recovery: -30, onBlock: -42`; `"HP Oniyama"`: `startup: 8, active: 23,
  recovery: -33, onBlock: -50`; `"EX Oniyama"`: `startup: 6, active: 25,
  recovery: -43, onBlock: -60`. This independently-built dataset matches
  `oro.json`'s Hit/Recovery/Block_advantage **exactly, including the
  negative sign, on all four strengths** (Startup uniformly +1 vs
  `oro.json`, the same counting-convention offset noted above).

**Reasoning:** two of three checked sources (`oro.json` itself + FAT-3S)
independently agree Recovery is negative for every Oniyama strength, with
FAT-3S matching magnitudes exactly. Only EventHubs (an older, 2011,
less rigorously maintained fan compilation) shows a positive Recovery.
Given two independently-maintained datasets agree on the negative sign,
the oracle's negative figures read as the community-consensus value, not
a scraping error — the harness's R metric (always >=0, a post-active
tick count) is structurally incapable of representing a negative
published field, exactly as the corpus's own classification states. This
is not primarily a value dispute between oracle and measurement but a
representational mismatch. The caveat: the *external* sources disagree
with *each other* on sign, not just with the oracle, so the STRUCTURAL
read here rests on a 2-of-3 majority rather than unanimous corroboration
(contrast with Akuma above, where both external sources agreed).

**Corpus-comment inaccuracy found during verification (does not affect
the verdict):** the block-entry's xfail string
(`tools/frame-data/corpus-oro.yaml:719`) says "Block_advantage='-34' for
this move family's Strong/Fierce rows" — `-34` is actually Jab's own
Block_advantage in `oro.json`, not Strong's (-42) or Fierce's (-50).
Flagged here as a comment inaccuracy, not corrected in the corpus per
this step's append-only/no-corpus-edit constraint.

**MASK-1 (2026-07-11):** the corpus entries `oro-oniyama-lp-whiff/-block/-hit`
now FIELD-MASK the incomparable R field (drop `expect.R`, keep S/A
asserted, both exact) citing this section; see synthesis §13.15. The
block leg's `adv` and the hit leg's `adv`/`kd` were already unasserted
before MASK-1 and remain so, unchanged (the incomparability comment
above already covers them; masking does not add a new assert).

**DATED PRIMARY-SOURCE UPDATE (2026-07-11, LAYER-1).** Arcade hardware
now settles the 2-of-3-majority caveat above with a primary source, not
another secondary compilation: `sfiii3nr1` (Japan 990512 NO CD) plays
busy-R = **29** on the Oniyama Jab whiff recipe (2 reps byte-identical;
f1=277334, box 277340-277349, busy768=277379, T=45, pre-move charge
frames correctly excluded -- `docs/arcade-frame-data/CAPTURE.md`'s new
Session 3 section), and a convention-twin engine-raw probe
independently measures 29 too. The oracle/FAT-3S magnitude (29, in
oracle notation -29) and EventHubs' positive 29 both match the hardware
count exactly; the harness's own measured 28 is now a confirmed
one-tick meter artifact, not "a clean, self-consistent reading" as this
section's Reasoning paragraph originally characterized it. The sign
question is unaffected and stays resolved as before: the oracle's
negative notation remains a representational choice the harness's R
metric (always >=0) cannot encode, so the STRUCTURAL verdict and MASK-1
(§13.15, `docs/frame-data-synthesis.md`) stand completely unchanged --
this update resolves the *magnitude* dispute (28 vs 29) with a primary
source, it does not touch the *sign* incomparability the STRUCTURAL
verdict was always about. Golden-pinned masked drift (28->29) is
EXPECTED whenever a future meter fix lands; no drift is predicted or
permitted by this dated update itself (comment-only, no corpus value
change).

---

## 3. Ibuki — Kubiori (motion hcf+P per corpus; see motion note below)

**Verdict: STRUCTURAL**

**Corpus citation:** section comment `tools/frame-data/corpus-ibuki.yaml:765-829`
(the "STRUCTURAL FINDING (two-phase move...)" sub-header is at line 790).
Three entries:
- `ibuki-kubiori-lp-whiff` (`tools/frame-data/corpus-ibuki.yaml:831`) and
  `-block` (`:838,843`): xfail cites the "residual item 18(b) whiff-R
  truncation" bucket — measured R=8 vs the oracle's first-triplet
  (Jab-strength) Recovery=15; S=14/A=11 assert exactly against the
  oracle's first-triplet values.
- `ibuki-kubiori-lp-hit` (`tools/frame-data/corpus-ibuki.yaml:871`): the
  true oracle-field-incomparable entry:

```
expect: { S: 14, A: 11, R: 15, kd: 1, xfail: "STRUCTURAL FINDING,
UNCLASSIFIED ... oracle-field-incomparable (Oro Oniyama precedent posture ...):
measured S=14/A=1/R=39 (self-consistent, T=54=S+A+R exactly, endrel=1) does
not match ibuki.json's published Kubiori row at any strength (Hit triplet
11/12/14, Recovery triplet 15/18/19 ...). HYPOTHESIS ONLY (not confirmed):
A=1 matches the universal Throw-family 'Hit=1' pattern ... suggesting the
published row describes a precursor strike phase ... rather than this
dummy:none path's real grab-catch-and-throw phase." }
```

**Oracle row:** `docs/arcade-frame-data/ibuki.json`, `"Kubiori"`:
`Startup: "14<br>15<br>17"`, `Hit: "11<br>12<br>14"`,
`Recovery: "15<br>18<br>19"`, `Block_advantage: "-10<br>-16<br>-19"`
(Jab/Strong/Fierce triplet in one row; `Throw_range: "40"`).

**Measured:**
- WHIFF/BLOCK (dummy stand, dist=40): S=14, A=11, R=8 (vs oracle
  first-triplet R=15).
- HIT (dummy:none, dist swept 10-40, all converge to the same
  signature): S=14, A=1, R=39, adv=+94, kd=1 (T=54=S+A+R exactly,
  endrel=1) — matches the oracle's Kubiori row at no strength.

**External sources (retrieved 2026-07-10):**
- EventHubs
  (`https://www.eventhubs.com/guides/2011/aug/22/ibukis-frame-data-street-fighter-3-third-strike/`):
  `"Neckbreaker | Parry: L | Damage: 150 | Stun: 15 | Cancel: — | Startup:
  14/15/17 | Active: 11/12/14 | Recovery: 15/18/19 | Frame Adv. Block:
  -16/-16/-19"`. Startup/Active/Recovery all match `ibuki.json`'s triplet
  exactly. Block-adv first value (-16) does not match `ibuki.json`'s -10
  (possible EventHubs transcription error — see FAT-3S below, which
  agrees with the oracle).
- FAT-3S (`Ibuki.moves.normal["Kubiori"]`): `{"startup": "15/16/18",
  "active": "2014-12-10T15:00:00.000Z", "recovery": "15/18/19", "onBlock":
  "-10/-16/-19", "plnCmd": "qcf+P"}`. Recovery and Block_advantage
  triplets match `ibuki.json` **exactly** (`-10/-16/-19` agrees with the
  oracle, contradicting EventHubs' `-16` first value). Startup is
  uniformly +1 vs oracle (same convention offset pattern as Oro above).
  The `active` field is corrupted — a literal spreadsheet
  auto-formatted date-string, almost certainly a Google-Sheets
  date-autocorrect artifact from an original "11/12/14" cell entry;
  unusable as a value, but its shape is independent circumstantial
  confirmation that the underlying figure was a slash-separated
  "11/12/14" triplet, matching EventHubs' Active exactly.

**Reasoning:** both EventHubs and FAT-3S independently reproduce the
oracle's WHIFF/BLOCK-leg figures (S=14/Hit=11/R=15, Jab strength) almost
exactly — the published Kubiori row is externally corroborated as
accurate for that phase of the move. The HIT-leg divergence
(S=14/A=1/R=39) is not found in any oracle or external source at any
strength; the corpus's own hypothesis — that the published row describes
a distinct, earlier strike/setup phase rather than the actual
catch-and-throw phase measured on HIT — is the best-supported read. This
is a genuine two-phase structural mismatch, not a wrong number in the
oracle.

**Side finding 1 (does not bear on the verdict): FAT-3S `active` field is
corrupted.** As noted above, FAT-3S's `active` value for Kubiori is the
literal string `"2014-12-10T15:00:00.000Z"` — a spreadsheet
date-autocorrect artifact, not usable data, but its date components
(`12`, `10`, `14`) circumstantially confirm the original cell held a
slash-separated `11/12/14`-shaped triplet matching EventHubs' Active
column.

**Side finding 2 (does not bear on the verdict): motion notation
conflict.** `tools/frame-data/corpus-ibuki.yaml:646` states "CONFIRMED
MOTION: Kubiori is hcf+P (Jab strength via LP), NOT hcf+K", based on
in-repo live motion probing (`:635-646`). FAT-3S's own `plnCmd` for this
exact move (`Ibuki.moves.normal["Kubiori"]`) is `"qcf+P"` (236+P), and an
independent GameFAQs Ibuki FAQ (by _Arlieth_,
`gamefaqs.gamespot.com/arcade/575310.../faqs/14080`, retrieved
2026-07-10) states Kubiori "uses the command QCF + Punch", explicitly not
HCF. This is recorded as a discrepancy, not resolved — verifying which
claim is correct would require repo-side live motion re-probing, which is
out of scope for this doc-only step.

**MASK-1 (2026-07-11):** the corpus entry `ibuki-kubiori-lp-hit` now
FIELD-MASKS the incomparable A and R fields (drops `expect.A`/`expect.R`,
keeps S=14 and kd=1 asserted as explicit literals, both exact) citing
this section; see synthesis §13.15. `ibuki-kubiori-lp-whiff`/`-block`
are explicitly **NOT masked** — their R divergence (measured 8 vs
oracle first-triplet 15) is residual item 18(b), a value divergence on
a comparable field (S=14/A=11 both assert exactly), not a structural
incomparability; both stay XFAIL unchanged.

---

## 4. Sean — Dragon Smash (Strong), WHIFF/BLOCK/HIT

**CAPTURE-1 UPDATE (2026-07-11): CLOSED, ORACLE-WRONG-CONFIRMED.** The
"not part of the closed bucket" / "user review pending" framing below is
the pre-capture history — preserved verbatim, dated, not rewritten. Live
arcade ground-truth capture (Fightcade FBNeo + Lua bridge,
`docs/arcade-frame-data/CAPTURE.md`) has since settled this: dp+MP on
real arcade hardware measures S=7/A=8/R=39, exactly matching this repo's
own live engine measurement and NOT `sean.json`'s declared Strong row
(S=5/A=6/R=36, byte-identical to Jab). The LP and HP flank captures the
same session landed exactly on `sean.json`'s own declared Jab/Fierce
rows, ruling out a rig-calibration explanation for the Strong miss. See
the "CAPTURE-1 confirmation" block below (after the pre-capture history)
for the full evidence and disposition; `tools/frame-data/corpus-sean.yaml`'s
three `sean-dragonsmash-mp-*` entries now assert the arcade-confirmed
literal value and are plain PASS.

**Pre-capture history (2026-07-10, preserved verbatim):** not part of the
closed 7-entry oracle-incomparable bucket. This item was tracked in the
corpus as an **open, unresolved "user review pending"**
item (`tools/frame-data/corpus-sean.yaml:920` — "Dragon Smash Strong
oracle-duplicate question on the user-review list w/ the interpolation
finding... User review pending (overnight autonomous run)"), not a closed
oracle-field-incomparable disposition. It is included here because it was
explicitly named for external-source verification, with its distinct,
still-open status called out clearly. **This section does not pick a
side.**

**Verdict (as of 2026-07-10, superseded above): UNVERIFIABLE** (external sources conflict with each other, not
just with the oracle)

**Corpus citation:** file-header finding
`tools/frame-data/corpus-sean.yaml:47-121` ("NEW FINDING: DRAGON SMASH
STRONG" through "DECISIVE INTERPOLATION FINDING"); section disposition
`tools/frame-data/corpus-sean.yaml:831-844`; entries
`sean-dragonsmash-mp-whiff` (`:866,871`), `-block` (`:873`), `-hit`
(`:880`):

```
expect: { S: from-qjson, A: from-qjson, R: from-qjson, xfail: "NEW FINDING,
UNEXPLAINED, flagged to orchestrator/user ...: measured S=7/A=8/R=39 vs
sean.json's own declared S=5/A=6/R=36 (IDENTICAL to Jab's own row) — a
uniform divergence present on WHIFF itself, isolated to this one strength
(Jab/Fierce both fully clean). ... HYPOTHESES ONLY, neither confirmed: (1) a
genuine engine defect ... or (2) sean.json's own Strong row is a data-entry
duplicate of Jab's row ..." }
```

**Oracle rows:** `docs/arcade-frame-data/sean.json`:

| Strength | Startup | Hit | Recovery | Block_advantage |
| --- | --- | --- | --- | --- |
| Jab | 5 | 6 | 36 | -24 |
| Strong | 5 | 6 | 36 | -24 (byte-identical to Jab) |
| Fierce | 9 | 9 | 42 | -34 |

**Measured:** Jab: S=5/A=6/R=36/adv=-24 (exact match to oracle, all three
outcomes). Strong: S=7/A=8/R=39 uniformly on WHIFF/BLOCK(adv=-30)/HIT —
does not match the declared Strong row, and interpolates almost exactly
between measured Jab and measured Fierce (S=7 is the exact midpoint of 5
and 9; R=39 is the exact midpoint of 36 and 42). Fierce: S=9/A=9/R=42/adv=-34
(exact match to oracle, all three outcomes).

**External sources (retrieved 2026-07-10):**
- EventHubs
  (`https://www.eventhubs.com/guides/2011/aug/22/seans-frame-data-street-fighter-3-third-strike/`):
  `"Dragon Smash Light Punch | HL | 130(90) | 11 | Su | 5 | 5 | 36 | -24"`,
  `"Dragon Smash Medium Punch | HL | 130(90) | 11 | Su | 7 | 7 | 39 |
  -30"`, `"Dragon Smash Hard Punch | HL | 130(90) | 11 | Su | 9 | 8 | 42 |
  -34"`. **This source shows Strong as a genuinely distinct row**
  (S=7/Active=7/R=39/Badv=-30), NOT identical to Jab. Applying the same
  +1 Hit-vs-Active convention offset independently confirmed on Jab
  (oracle Hit=6 vs EventHubs Active=5) and Fierce (oracle Hit=9 vs
  EventHubs Active=8) to Strong (EventHubs Active=7 → oracle-convention
  Hit=8) produces an exact match to this engine's measured Strong:
  S=7/A=8/R=39/adv=-30, all four fields.
- FAT-3S (`Sean.moves.normal`): `"LP Dragon Smash"`: `startup: 6, active:
  6, recovery: 36, onBlock: -24`; `"MP Dragon Smash"`: `startup: 6,
  active: 6, recovery: 36, onBlock: -24` (byte-identical to LP, agreeing
  with `sean.json`'s duplicate-row reading, NOT with EventHubs' distinct
  Strong row); `"HP Dragon Smash"`: `startup: 10, active: 9, recovery:
  42, onBlock: -34`. FAT-3S's Startup is uniformly +1 vs `sean.json`
  (same convention-offset pattern seen on Oro/Ibuki above); Active/
  Recovery/onBlock otherwise track `sean.json` closely on Jab/Fierce, and
  it reproduces the Jab=Strong duplicate.

**Reasoning: three-way split, do not pick a side.** `sean.json` itself
and FAT-3S (two independently-maintained datasets) agree Jab and Strong
Dragon Smash are byte-identical rows. EventHubs (2011, older) disagrees
and shows a distinct Strong row whose numbers — after correcting for a
convention offset independently verified on this same source's own
Jab/Fierce rows — match this engine's own live-measured Strong values
exactly on S/R/adv and are within the known convention gap on A. No
fourth source was found, nor any changelog/issue history on
`Coccis77/thirdstrikedatabot` (single commit, "Add some json files", no
issues) that would break the tie. Both hypotheses recorded in the corpus
(engine defect vs. oracle data-entry duplicate) remain formally open;
external evidence does not resolve which is correct — it does show that
"the engine defect coincidentally lands on the exact Jab/Fierce
arithmetic midpoint" is not the only way to read the numbers, since a
real, non-duplicate Strong row (matching EventHubs, once the convention
is applied) fits the live measurement just as well. **Verdict:
UNVERIFIABLE.**

**MASK-1 (2026-07-11):** `sean-dragonsmash-mp-whiff/-block/-hit` are
explicitly **NOT masked** — the UNVERIFIABLE verdict above is a live
three-way value dispute, not a structural incomparability, and MASK-1's
membership rule (synthesis §13.15) bars masking any value dispute.
All three entries stay XFAIL, unchanged; the pending arcade capture
will rule.

**CAPTURE-1 confirmation (2026-07-11).** The pending arcade capture named
above has now run. Rig: Fightcade FBNeo (bundled Wine, macOS) + a Lua
bridge script, live sfiii3 (Euro 990608), savestate-anchored, 2
byte-identical repetitions per capture (full methodology:
`docs/arcade-frame-data/CAPTURE.md`). Result (`docs/arcade-frame-data/
CAPTURE.md`'s per-row table; capture session 2, target 1):

| Strength | Arcade (captured) | `sean.json` (oracle) | This engine (measured) |
| --- | --- | --- | --- |
| Jab | S=5/A=6/R=36 | S=5/A=6/R=36 | S=5/A=6/R=36 (exact) |
| Strong | **S=7/A=8/R=39** | S=5/A=6/R=36 (Jab-duplicate) | S=7/A=8/R=39 (exact) |
| Fierce | S=9/A=9/R=42 | S=9/A=9/R=42 | S=9/A=9/R=42 (exact) |

Jab and Fierce both replay their own declared oracle row exactly on real
hardware, ruling out a capture-rig or counting-convention miscalibration
as the explanation for Strong's miss. Strong on real arcade hardware
plays S=7/A=8/R=39 — **the exact value this repo's engine has measured
all along**, not the value `sean.json` publishes. This settles the
three-way split above: `sean.json`'s (and FAT-3S's, which copies it)
Strong row is a genuine data-entry duplicate of Jab's row, not a real
arcade value; EventHubs' independently-derived, convention-corrected
Strong row (matching this engine's measurement, as already noted above)
is the one that was right.

**Verdict upgraded: UNVERIFIABLE → ORACLE-WRONG-CONFIRMED.** This is the
first (and, per this register's own verdict vocabulary, the *only
possible*) way an `expect` value in this harness moves onto a measured
engine value without a display-convention adoption: the reference itself
is proven wrong by a primary hardware source, not merely disputed by
secondary compilations. `tools/frame-data/corpus-sean.yaml`'s three
`sean-dragonsmash-mp-*` entries now assert `S: 7, A: 8, R: 39` (and
`adv: -30` on the BLOCK leg) as explicit literals, `xfail` removed —
plain PASS on all three. This is item 4's only member and it is now
fully closed; no further action remains open for Dragon Smash.

---

## 5. Sean — Zenten / Sean Roll (Jab/Strong/Fierce), WHIFF ×3 (new 2026-07-11, MASK-1)

**Verdict: STRUCTURAL**

**Corpus citation:** file-header ZENTEN / SEAN ROLL block
`tools/frame-data/corpus-sean.yaml:184-217`; section comment `:1139-1148`;
entries `sean-roll-lp-whiff` (`:1150-1155`), `sean-roll-mp-whiff`
(`:1157-1162`), `sean-roll-hp-whiff` (`:1164-1169`):

```
expect: { R: from-qjson, xfail: "R incomparable — zero-active movement-only
dodge, whole duration classifies as saturated startup (measured S=T=28, A=0,
R=0; T=28 exactly matches sean.json's own Startup+Hit+Recovery sum 2+19+7).
Overlay R counts post-active ticks and there is no active anchor for a
zero-active move. Measurement-semantics gap (same review ruling as
corpus-akuma.yaml's akuma-ashura-whiff, 2026-07-08), NOT a timing divergence
— see file header." }
```

**Oracle rows:** `docs/arcade-frame-data/sean.json`, `"Sean Roll
(Jab/Strong/Fierce)"`: `Startup: "2"`, `Hit: "19"/"29"/"43"`,
`Recovery: "7"` (all three strengths), `Damage: "-"`, `Parry:
{High: false, Low: false}` — every non-S/Hit/Recovery field non-numeric,
confirming a non-damaging dodge, not an attack.

**Measured:** a real FINAL fires with ZERO active frames on all three
strengths (`first_active_raw=-1`, `engine_a=0`, `A=0`) — the roll never
produces a hitbox. Measured S=T=28/38/52 (Jab/Strong/Fierce), A=0, R=0.
The measured total raw duration T exactly matches the SUM of `sean.json`'s
own Startup+Hit+Recovery fields for each strength (Jab: 28=2+19+7;
Strong: 38=2+29+7; Fierce: 52=2+43+7) — the same zero-active-frame
degenerate-decomposition shape as Akuma's Ashura Senkuu (§1 above): with
no active window to anchor S/A/R's normal decomposition, the whole
duration classifies as pure "S" in the overlay's own broken
decomposition.

**External sources (retrieved this session from the cached FAT-3S
snapshot, `Sean.moves.normal`):** `"LP Sean Roll"`: `startup: 3, active:
19, recovery: 7`; `"MP Sean Roll"`: `startup: 3, active: 29, recovery:
7`; `"HP Sean Roll"`: `startup: 3, active: 43, recovery: 7` (all three
tagged `moveType: "movement-special"`, confirming FAT-3S independently
classifies Roll as a non-attack movement special, not a normal/special
attack). FAT-3S's `active` field matches `sean.json`'s Hit field
**exactly** on all three strengths (19/29/43), and its `recovery`
matches `sean.json`'s Recovery **exactly** (7, uniform). Startup is
uniformly +1 vs `sean.json` (3 vs 2) — the same convention-offset
pattern already independently confirmed on Oro (§2) and Ibuki (§3)
above. No EventHubs Sean Roll row was found (EventHubs' Sean frame-data
guide, already checked for §4 above, does not list Sean Roll — it is a
movement special, not one of the guide's listed attacks). One external
source (FAT-3S) corroborates the oracle's Recovery=7 figure exactly; no
second, independent source was found for this specific move. This is
weaker corroboration than §1's two-source agreement, but the class-based
argument below does not depend on the figure's correctness — only on
the harness's own representational limit.

**Reasoning:** this is the same zero-active-frame representational
gap as §1 (Akuma Ashura Senkuu): the harness's S/A/R decomposition,
anchored on an active-hitbox window, has no way to represent a move
that never produces a hitbox — the incomparability is a fact about the
harness's own measurement model, independent of whether `sean.json`'s
published Recovery=7 is itself correct. FAT-3S's exact match on
Recovery (and on Hit/active) for all three strengths is additional,
found-not-searched-for corroboration that the published figure is not
itself in dispute, strengthening rather than merely restating the
class-based argument — but even absent that match, §1's own closing
note already frames this as a harness-capability question, not a data
question, so external retrieval was not treated as a hard dependency
for this verdict (per the orchestrator's ruling on this point).

---

## 6. Urien — Headbutt (Jab/Strong/Fierce), WHIFF (new 2026-07-11, RE-ANCHOR-1 OUTCOME A)

**Verdict: ORACLE-WRONG-CONFIRMED** (instances 3-5 of the class, after
Sean Dragon Smash Strong — §4).

`urien.json`'s Headbutt row publishes Recovery **per strength, never a
uniform figure**: Jab=19, Strong=18, Fierce=19. RE-ANCHOR-1 (docs/
frame-data-synthesis.md §13.17) proved, per strength, against each
strength's OWN oracle value, that the published figure counts a 3-frame
non-actionable-claimed residue that is in fact actionable:

- **Session 4** (`docs/plan-frame-data-completion.md` OVERLAY RE-ANCHOR
  §6.1/§7 Step 2; raw capture `<sp>/capture/session4-results.md`
  Target 1): live arcade hardware (sfiii3nr1, 990512 NO CD, 2 reps
  byte-identical each strength) measures busy-edge R = 16/15/16 for
  Jab/Strong/Fierce respectively — 3 below each strength's own oracle
  figure, uniformly.
- **Session 5** (`<sp>/capture/session5-results.md`, the actionability
  probe): from 2 ticks before the measured busy edge, an alternating
  LP/MP mash accepts a NEW move exactly at the busy edge (`edge+0`) on
  all three strengths — not at `edge+3` — confirming the busy edge IS
  the character's first-actionable frame, not merely a guard-availability
  edge. A validity control on the same session (`ibuki-kazekiri-lk-whiff`,
  an already-arcade-exact busy-edge PASS row) confirms the probe measures
  actionability rather than routine end.

**Disposition:** each strength's whiff-leg R re-baselines from its own
oracle figure to its own arcade-confirmed value —
`urien-headbutt-lp-whiff` 19->16, `-mp-whiff` 18->15, `-hp-whiff` 19->16
(`tools/frame-data/corpus-urien.yaml`, dated §13.17 comments). The six
BLOCK/HIT contact legs keep their measured values (block 19/18/19, hit
19/18/19) unchanged — lever N never applies to contact outcomes, and whether
arcade's own contact-leg R is also 3 lower is a SEPARATE, unmeasured
question, left explicitly open here, not assumed either way; `from-qjson`
is no longer citable for those legs' R either (same underlying oracle
field), so they convert to literals at their current value, citation-only.

## 7. Ibuki — Towards+Forward / Towards+Roundhouse, WHIFF (new 2026-07-11, RE-ANCHOR-1 OUTCOME A)

**Verdict: ORACLE-WRONG-CONFIRMED** (instances 6-7 of the class).

`ibuki.json`'s "Towards + Forward" (Recovery=4) and "Towards + Roundhouse"
(Recovery=21) rows were the two currently-PASS, arcade-exact windows the
RE-ANCHOR-1 census (`<sp>/reanchor/census-report.md`, Criterion 1) found
lever N would move away from their oracle value — the same E7/E6 "moving
a currently-arcade-exact PASS value" death shape any other lever hits.
Re-diagnosis (`<sp>/reanchor/leverN-rediagnosis.md`) found the SAME
signature as the Urien Headbutt trio above (oracle == overlay-natural-end
== busy-edge+3, an identical 3-tick post-edge `rno3==3` residue) and the
SAME Session-5 actionability probe result: both rows accept a new move
exactly at the busy edge (`edge+0`), confirming the oracle's 4/21 count
the same 3-frame actionable residue as the headbutt trio.

**Disposition:** `ibuki-twdsforward-whiff` R 4->1,
`ibuki-twdsroundhouse-whiff` R 21->18 (`tools/frame-data/corpus-ibuki.yaml`,
dated §13.17 comments). The corresponding BLOCK legs
(`ibuki-twdsforward-block` R=4, `ibuki-twdsroundhouse-block` R=22) keep
their measured values unchanged — same citation-only conversion as the
headbutt contact legs, same open contact-leg question left unmeasured.

---

## 8. Akuma — Universal Overhead (UOH, `press MP+MK`), BLOCK/HIT contact legs (new 2026-07-13, UOH-CLOSURE)

**Verdict: ORACLE-WRONG-CONFIRMED** (instance 8 of the class, after Ibuki
Towards+Forward/Towards+Roundhouse — §7).

**Corpus citation:** `tools/frame-data/corpus-akuma.yaml:516-525`
(`akuma-uoh-block` correction) and `:541-550` (`akuma-uoh-hit`
correction); entries at `:526-531` and `:551-556`.

**Oracle row:** `docs/arcade-frame-data/akuma.json`, `"Universal
Overhead"`: `Startup: "15"`, `Hit: "8"`, `Recovery: "7"` — one Recovery
figure for the whole move, no per-outcome (WHIFF/BLOCK/HIT) breakdown.
This figure is confirmed correct for WHIFF (arcade 990512 R=7 exactly,
`docs/arcade-frame-data/CAPTURE.md` Session 3, cited in
`akuma-uoh-whiff`'s own corpus note) — the divergence below is entirely
about the two CONTACT legs, not a WHIFF dispute.

**Previously shipped (PASS, both wrong):** `akuma-uoh-block` displayed
R=7 — the shipped engine's legacy `recovery_pf` tally, which happened to
equal the whiff-canonical oracle figure by coincidence, not by any
verified contact-leg measurement. `akuma-uoh-hit` displayed R=5 — a
`§13.10` "Class 2" convention value, constructed as the oracle's whiff
figure (7) minus a 2-tick hitstun/blockstun cut-window compression
believed (not measured) to be UOH-specific.

**Arcade measurement (`<sp>/zero/arcade-s7/session-report.md`, Session 7,
sfiii3nr1 Japan 990512 NO CD):** 2 reps byte-identical each leg,
`akuma-uoh-block` actionability-probe-confirmed (accepts a new input
exactly at the busy edge, fc 247479, zero early acceptance). Real
hardware measures **BLOCK R=8, HIT R=6** — matching this engine's own
busy-edge/lever-R construction (`fd_lever_r_applies`/lever T,
`frame_data_overlay.c`) exactly on both legs, with HIT landing exactly
one tick below BLOCK's corrected value (the genuine HIT/BLOCK
differential, not the whiff-derived one the old §13.10 convention
assumed). Neither corrected value (8, 6) equals either the oracle's
single Recovery figure (7) or the two legacy/convention values that were
shipping as PASS.

**Reasoning:** both of Akuma's UOH contact legs were shipping as PASS
against values that were never independently verified against real
hardware — one a coincidental match to the (WHIFF-only) oracle figure,
the other a constructed convention layered on top of that same
oracle-derived assumption. A primary arcade capture directly measuring
both contact legs shows both assumptions wrong, in the same direction
lever T's busy-edge construction independently predicted from engine
signals alone (`hstop_in_box`, `docs/frame-data-synthesis.md` §13.20
§3.3). This is the same "wrong-data-passing" shape as the Urien Headbutt
precedent (§6 above) — a currently-PASS row whose value was never
actually confirmed against hardware until this session, not a fresh
disagreement about which value is right.

**Disposition:** `akuma-uoh-block` R 7->8, `akuma-uoh-hit` R 5->6
(`tools/frame-data/corpus-akuma.yaml`, dated 2026-07-13 §13.20
comments), both re-literaled at the arcade value, PASS unaffected by the
lever-T wire-in (the displayed busy-edge value now IS the corrected
literal).

**Family-wide note (UOH-CLOSURE, `docs/frame-data-synthesis.md` §13.20):**
Akuma is one instance of a program-wide finding, not an isolated
character quirk. Every UOH contact-leg row this program measured against
real hardware — Akuma/Yun/Ibuki (Session 7), and the "XFAIL-18" bucket
of Alex/Dudley/Elena/Necro/Twelve/Urien/Remy/Yang/Yun (Sessions 7 and 9,
18 rows total) — sided with the engine's own busy-edge/lever-R
construction and NOT with the whiff-canonical oracle figure (or any
convention derived from it) on every single row tested (33 of 33
verified UOH contact windows, zero exceptions once Session 11 resolved
the six BLOCK legs Session 8 had misread as whiff-shaped captures — see
§13.20 §2 for that forensic correction). **UOH contact R was never a
whiff-canonical quantity** — the pre-UOH-CLOSURE XFAIL-18 corpus
comments' own framing (measured busy-edge value vs. "expected"
whiff-derived oracle figure) had the direction of the discrepancy right
but its cause wrong: the oracle was never describing the contact leg to
begin with. See `docs/frame-data-synthesis.md` §13.20 for the complete
33-row disposition table (§4.1/§4.2) and the per-character hardware
citations; this entry does not repeat that table, only Akuma's own two
rows (the only member of the "currently-PASS, silently wrong" sub-class
sharing Urien Headbutt's exact shape closely enough to warrant its own
ERRATA entry — the other six PASS-corrected rows, Ibuki/Oro/Sean/Q, are
cited directly in their own corpus files' dated comments per the same
session evidence).

---

## 9. Oro — Jinchu Nobori (Short/EX), BLOCK/HIT contact legs (new 2026-07-14, ENGINE-JINCHU)

**Verdict: ORACLE-WRONG-CONFIRMED** (upgraded from the plan's pre-registered
ORACLE-LIKELY-WRONG by the opus-substitute diff review, 2026-07-14, and queued
for Fable re-review): the evidence is a single primary-source real-hardware
capture directly measuring all four legs (Session 6, sfiii3nr1 Japan 990512,
derive2.py, 2 reps/leg byte-identical) — the same evidence class items 4/6/7/8
carry as CONFIRMED (a primary source directly measuring the move), and item 8
(Akuma UOH) is the exact analog on the same rig/method/contact-leg-re-anchor
shape. Oracle 19/16 is contradicted by hardware 33/33/34/52 (no field matches).
The verdict is about the oracle being wrong; the EX-hit engine-vs-arcade 1-frame
gap is a separate overlay-fidelity note and does not soften it.

**Corpus citation:** `tools/frame-data/corpus-oro.yaml` (`oro-jinchu-lk-block`,
`oro-jinchu-lk-hit`), `tools/frame-data/corpus-oro-ex.yaml`
(`oro-exjinchu-block`, `oro-exjinchu-hit`) — all four RE-ANCHORED 2026-07-14
to Session 6's arcade literal, superseding the ENGINE-8 closure that
preserved the (falsified) oracle value.

**Oracle rows:** `docs/arcade-frame-data/oro.json`: "Jinchu Nobori (Short)"
(:1578) Recovery=**19**; "Jinchu Nobori (EX)" (:1683) Recovery=**16**. One
`Recovery` field per move, no per-outcome (WHIFF/BLOCK/HIT) breakdown — the
same shape as items 6/7's Headbutt/Towards-family rows.

**Previously shipped/closed value (ENGINE-8, 2026-07-11, ARCADE-WHIFF-CANONICAL
/ NO-CODE-FIX-EXISTS):** all four contact legs (base BLOCK/HIT, EX BLOCK/HIT)
stayed XFAIL asserting the oracle's whiff-canonical Recovery (19 base, 16 EX)
with engine R=0 (no in-engine recovery anchor — the move ends by airborne
`Player_normal` dispatch handoff). ENGINE-8 exhaustively measured every
candidate end-anchor in the engine's own state (r1-clear, chart-idle,
touchdown, stable-idle-loop) and closed NO-CODE-FIX-EXISTS: none of them
landed on 19.

**Arcade measurement (Session 6, `<sp>/zero/arcade-track-a/session-report.md`,
sfiii3nr1 Japan 990512 NO CD, `derive2.py`, 2 reps/leg byte-identical):** base
LK (act 0005003E): HIT R=**33**, BLOCK R=**33** (symmetric). EX (act
00050041, meter-forced): HIT R=**34**, BLOCK R=**52** (asymmetric, block>hit
by 18f). None of the four arcade values equals either oracle Recovery figure
(19/16).

**Reasoning:** the oracle's single `Recovery` field is structurally unable to
express the EX variant's 34(HIT)/52(BLOCK) split at all — even setting aside
whether 19/16 were ever right, the field's own shape cannot hold two
divergent contact-outcome values for one move, the same structural gap as
items 6/7's per-outcome Headbutt/Towards-family rows. Base-LK's own
symmetric 33/33 also does not match the oracle's 19. A primary-source,
real-hardware capture measuring all four legs directly (not a secondary
compilation) is the strongest evidence class this register recognizes (cf.
items 4/6/7/8, all confirmed by exactly this evidence shape); it falsifies
the oracle Recovery on both variants and both outcomes.

**Disposition:** `oro-jinchu-lk-block` R 19->33 (arcade-literal, stays
XFAIL — the engine's busy `771->768` edge never latches on this leg,
`busyr=-1`, mechanically unmeasurable via any busy-edge instrument);
`oro-jinchu-lk-hit` R 19->33 (arcade-literal, engine-exact via the new lever
U, `fd_jinchu_bounce_recovery_r` — **converts to PASS**); `oro-exjinchu-block`
R 16->52 (arcade-literal, stays XFAIL, same unmeasurable-busy-edge
disposition); `oro-exjinchu-hit` R 16->34 (arcade-literal, stays XFAIL — the
engine's lever-U busy edge reads 33, one frame short of arcade 34, a genuine
divergence, not a code-fixable miss — lever U fires on this leg too since no
engine signal separates it from the base-LK HIT leg, but the golden/expect
split (33 engine-truth vs 34 arcade-truth) keeps it correctly XFAIL, not
XPASS). Full disposition table and per-leg discriminating condition:
`docs/frame-data-synthesis.md` §13.21 (lever U record).

**Supersedes:** ENGINE-8's 2026-07-11 "CLOSED — ARCADE-WHIFF-CANONICAL /
NO-CODE-FIX-EXISTS" verdict (`docs/plan-frame-data-completion.md` ENGINE-8
row; section comment `tools/frame-data/corpus-oro.yaml` Jinchu Nobori (Short)
header) is **FALSIFIED for the value it preserved** (oracle R=19/16) —
Session 6 shows arcade truth is 33/33 base, 34/52 EX. ENGINE-8's own
MECHANISM finding (no in-engine recovery anchor; airborne `Player_normal`
handoff; every candidate end-anchor measured, none matching) stands
unchanged and remains exactly why the BLOCK legs (and the EX-hit's own
1-frame gap) cannot be closed by any busy-edge lever — see
`docs/plan-frame-data-completion.md`'s updated ENGINE-8 row.

---

## Summary

| # | Move | Bucket status | Verdict |
| --- | --- | --- | --- |
| 1 | Akuma Ashura Senkuu (WHIFF) | Closed, GRANTED (7-entry bucket) | STRUCTURAL |
| 2 | Oro Oniyama Jab (WHIFF/BLOCK/HIT, 3 entries) | Closed, GRANTED (7-entry bucket) | STRUCTURAL |
| 3 | Ibuki Kubiori (WHIFF/BLOCK item-18(b); HIT oracle-incomparable, 3 entries) | Closed, GRANTED (7-entry bucket) | STRUCTURAL |
| 4 | Sean Dragon Smash Strong (WHIFF/BLOCK/HIT, 3 entries) | **CLOSED 2026-07-11 (CAPTURE-1), converted to PASS in the corpus** | **ORACLE-WRONG-CONFIRMED** (was UNVERIFIABLE 2026-07-10) |
| 5 | Sean Zenten / Sean Roll (Jab/Strong/Fierce, WHIFF, 3 entries) | NEW 2026-07-11, MASK-1 masking bucket (not the original 7-entry bucket) | STRUCTURAL |
| 6 | Urien Headbutt (Jab/Strong/Fierce, WHIFF, 3 entries) | **NEW 2026-07-11 (RE-ANCHOR-1 OUTCOME A), converted to literal PASS in the corpus** | **ORACLE-WRONG-CONFIRMED** |
| 7 | Ibuki Towards+Forward / Towards+Roundhouse (WHIFF, 2 entries) | **NEW 2026-07-11 (RE-ANCHOR-1 OUTCOME A), converted to literal PASS in the corpus** | **ORACLE-WRONG-CONFIRMED** |
| 8 | Akuma Universal Overhead (BLOCK/HIT contact legs, 2 entries) | **NEW 2026-07-13 (UOH-CLOSURE), re-literaled to the arcade value in the corpus** | **ORACLE-WRONG-CONFIRMED** |
| 9 | Oro Jinchu Nobori (Short/EX), BLOCK/HIT contact legs (4 entries: 1 converts to PASS via lever U, 3 stay XFAIL at the arcade literal) | **NEW 2026-07-14 (ENGINE-JINCHU), re-literaled to the arcade value in the corpus; supersedes ENGINE-8** | **ORACLE-WRONG-CONFIRMED** (Session-6 primary hardware capture; same evidence class as rows 4/6/7/8; queued for Fable re-review) |

Tally: **4 STRUCTURAL + 5 ORACLE-WRONG-CONFIRMED**
(rows 4, 6, 7, 8 CONFIRMED — was 4 STRUCTURAL + 1 UNVERIFIABLE prior to
CAPTURE-1, 2026-07-11 — see row 4's CAPTURE-1 confirmation block above; rows
6-7 added the same day by RE-ANCHOR-1's Session 4/5 hardware actionability
capture, `docs/frame-data-synthesis.md` §13.17; row 8 added 2026-07-13 by
UOH-CLOSURE's Session 7 arcade capture, `docs/frame-data-synthesis.md`
§13.20; row 9 added 2026-07-14 by ENGINE-JINCHU's Session 6 arcade capture,
`docs/frame-data-synthesis.md` §13.21 — LIKELY rather than CONFIRMED per the
plan's own pre-registration, pending Fable re-review of that label choice).

**MASK-1 (2026-07-11):** the STRUCTURAL rows (#1-#3, #5 — 8 entries: 1
Akuma + 3 Oro + 1 Ibuki HIT-leg + 3 Sean Roll) now FIELD-MASK their
incomparable field(s) in the corpus, asserting every comparable field
and reading `PASS`; see `docs/frame-data-synthesis.md` §13.15 for the
normative mechanism and member table. Row #3's WHIFF/BLOCK legs
(`ibuki-kubiori-lp-whiff/-block`) are item 18(b), not part of this
STRUCTURAL row's masked members. Row #4 (now
**ORACLE-WRONG-CONFIRMED** — see Summary row 4 above; was UNVERIFIABLE
prior to CAPTURE-1) is explicitly excluded from masking — see its own
per-section rider above. (It was never a masking candidate either way:
its entries converted to explicit-literal PASS instead, not a masked
PASS — see the CAPTURE-1 confirmation block in §4 below.)

## Closing note — what would close each open item

- **Akuma Ashura Senkuu:** closed as far as external corroboration goes
  (two independent sources agree on Recovery=9). What remains open is
  purely a harness capability question, not a data question: the S/A/R
  overlay would need a defined convention for zero-active moves (e.g. an
  explicit "no active window" sentinel distinct from S=T saturation)
  before this entry could ever flip from xfail to PASS. No further
  external research would change the verdict.
  **DATED RIDER (2026-07-11, MASK-1, append-only):** the harness gained
  exactly this kind of convention two days later — not a zero-active
  sentinel, but per-entry field masking (§13.15): `akuma-ashura-whiff`
  now drops the incomparable R assert, keeps `outcome=WHIFF`/`finals=1`
  asserted, and reads plain PASS (`docs/frame-data-synthesis.md` §13.15
  member table row 1; this document's Summary table row 1 and MASK-1
  note above). "before this entry could ever flip from xfail to PASS"
  no longer holds — it has flipped. The original closing-note text
  above is preserved verbatim as the record of what would have been
  needed under the old (pre-MASK-1) convention.
- **Oro Oniyama:** closed as far as this repo's disposition goes (2-of-3
  majority on the negative-Recovery sign). What would fully settle the
  EventHubs-vs-{oracle,FAT-3S} sign disagreement is a primary source —
  a frame-by-frame video capture of an Oniyama whiff/block, counted by
  hand from the recovery's first actionable frame to move-end — since
  none of the three sources checked here is a primary capture.
- **Ibuki Kubiori:** closed as an incomparability finding, but the
  two-phase hypothesis (published row = precursor strike phase, not the
  catch-and-throw phase) remains a hypothesis, not a confirmed mechanism.
  Closing it fully would need either an arcade-original source that
  explicitly documents Kubiori as a two-hit (strike-then-grab) move, or
  a repo-side trace that identifies a distinct hitbox/state transition
  matching the oracle's 11/12/14 Hit-triplet inside the throw's startup
  window. The motion side-finding (hcf+P vs qcf+P) would need a fresh
  repo-side live motion probe against both candidate inputs to resolve,
  independent of the oracle question.
- **Sean Dragon Smash Strong: CLOSED 2026-07-11.** The primary source
  named in (a) below has now been obtained — a live arcade capture
  (`docs/arcade-frame-data/CAPTURE.md`) settling the question directly:
  Strong Dragon Smash plays S=7/A=8/R=39 on real hardware, distinct from
  Jab and matching this engine's own measurement, not `sean.json`'s
  declared (Jab-duplicate) row. `sean.json`'s Strong row is confirmed a
  data-entry error. (Original closing-note text, preserved: closing it
  needed either (a) a primary source — a frame-by-frame capture (video or
  a rip of the arcade ROM's own hitbox/state timers) of a Strong Dragon
  Smash, which would settle whether Strong is truly distinct from Jab in
  the original game, or (b) evidence of `Coccis77/thirdstrikedatabot`'s
  own upstream source/methodology that would explain the duplication —
  (a) is what was obtained.)
