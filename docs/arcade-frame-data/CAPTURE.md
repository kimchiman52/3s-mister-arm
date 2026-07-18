# Arcade Ground-Truth Capture — Methodology and Results

**Status: rig validated, two sessions run, 2026-07-11 (CAPTURE-1).** This
document is the permanent repo home for the arcade-capture methodology
first built and run as part of CAPTURE-1 (see the tracker row in
`docs/plan-frame-data-completion.md`). It condenses the session's
scratchpad artifacts (feasibility report, capture logs, sweep results)
into one reproducible reference — every number below traces back to a
raw per-frame log or a specific commit/corpus citation.

## Purpose

Everything else in `docs/arcade-frame-data/` and `tools/frame-data/`
compares this repo's live engine measurement against a *secondary*
oracle (`Coccis77/thirdstrikedatabot`'s scraped JSON, cross-checked
against other fan compilations like EventHubs and FAT-3S). None of those
sources is the arcade PCB itself. Several open disputes in
`docs/frame-data-synthesis.md` (item 18, ENGINE-5, the Sean Dragon Smash
Strong question) ultimately turn on "what does the original arcade
hardware actually do" — a question no secondary source can answer.

This capture rig runs the actual arcade ROM (`sfiii3`, Street Fighter III
3rd Strike: Fight for the Future, Euro 990608) under FBNeo with
frame-accurate Lua instrumentation, so specific disputed rows can be
measured directly against real arcade behavior instead of a scraped
table.

## Rig

- **Emulator:** Fightcade's bundled FBNeo build
  (`/Applications/FightCade2.app/Contents/MacOS/emulator/fbneo/fcadefbneo.exe`,
  a 32-bit Windows PE) run under Fightcade's own bundled Wine
  (`wine32on64`, WINEARCH=win32, prefix
  `Contents/Resources/.wine32`) on macOS/arm64 via Rosetta 2. This is
  the same FBNeo core Fightcade uses for online arcade netplay — not a
  custom or patched build.
- **ROM:** `sfiii3.zip` from Fightcade's own ROM set, boots to the
  window title "Street Fighter III 3rd Strike: Fight for the Future
  (Euro 990608)".
- **Working directory:** a private shadow directory
  (`capture/fbneo-workdir/`) made of symlinks into the app's real
  `fbneo/` directory, with its own `config/`, `savestates/`,
  `fbneo-training-mode/`, `recordings/`, `screenshots/` — every FBNeo
  path is cwd-relative, so nothing is ever written into the user's real
  Fightcade install.
- **Launch command** (exact, from the feasibility report):
  ```
  cd <workdir> && \
  WINEARCH=win32 WINEPREFIX=/Applications/FightCade2.app/Contents/Resources/.wine32 \
  DYLD_FALLBACK_LIBRARY_PATH="/Applications/FightCade2.app/Contents/Resources/wine/lib:/Applications/FightCade2.app/Contents/Resources/wine/lib64:/opt/local/lib:/usr/lib:/usr/libexec:/usr/lib/system:/opt/X11/lib" \
  WINEDEBUG=-all \
  /Applications/FightCade2.app/Contents/Resources/wine/bin/wine32on64 "<workdir>/fcadefbneo.exe" sfiii3 bridge.lua
  ```
  The exe path must be absolute — a relative path makes Wine look in
  `system32` instead and fails.

## Control channel (`bridge.lua`)

A Lua 5.1 script loaded via FBNeo's own CLI loader (any bare `.lua`
argument at startup — `fightcade-fbneo/src/burner/win32/main.cpp:989-993`,
upstream `fightcadeorg/fightcade-fbneo`). No GUI automation, no
Fightcade training-mode quark (that hangs offline waiting for a GGPO
relay). The script:

- Polls a `bridge-cmd.txt` file every frame (`emu.registerbefore`) for a
  `"<seq>\n<lua chunk>"` command, executes it, and acks to
  `bridge-ack.txt`. Chunks can install per-frame `on_before`/`on_after`
  hooks for logging.
- Injects inputs via `joypad.set{["P2 Weak Punch"]=true, ...}` (key
  names dumped live from the emulator).
- Uses `savestate.create()/save()/load()` for an anchor/reload capture
  loop — every capture in the sweep below re-loads the same in-match
  neutral savestate, so repeated captures start from byte-identical
  state.
- Reads live game memory via the address map from
  `grouflon/3rd_training_lua` (cloned locally for reference): P1 object
  at `0x02068C6C`, P2 object at `0x02069104`; per-object fields used
  here: `action` dword at `base+0xAC`, `busy` word at `base+0x3D1`,
  `recovery flag` byte at `base+0x3B`, `freeze/hitstop` byte at
  `base+0x45`, attack-box pointer at `base+0x2C8` (4 slots x 8 bytes),
  character-id word at `base+0x3C0`.

## Frame-counting convention

Per-frame log fields, sampled post-frame (`emu.registerafter`):
`fc act ext mov rec rflag busy frz box x`.

- **Move frame 1** = the frame where `busy` transitions 0→3 *with the
  move's own act id already present* — not simply "the first frame with
  `act != 0`". Motion moves are routinely preceded by non-zero pre-move
  acts (walking/crouching during the motion's own hold-chain, `act !=
  0` while `busy` is still `0`) that are not part of the move at all. In
  the Sean Dragon Smash Strong capture (`cap-sean-dsmp-r1.log`),
  fc=22726-22728 read `act=00000002` (walk) and fc=22729-22733 read
  `act=00000006` (crouch), both with `busy=0`, before the move's own
  act id (`act=00050041`) and `busy=3` first appear together at
  fc=22734. A literal "first frame with `act != 0`" reproducer would
  anchor move frame 1 on fc=22726 instead of fc=22734 and, against the
  first box>0 frame (fc=22741), wrongly derive S=15 for this move
  instead of the correct S=7 reported below.
- **S** (startup) = frames from move frame 1 up to, but not including,
  the first frame with `box > 0`.
- **A** (active) = number of consecutive frames with `box > 0`.
- **R** (recovery) = frames strictly between the last `box > 0` frame
  and the frame where `busy` transitions 771→768 (the first fully
  actionable frame is excluded, same convention as the engine's own
  S+A+R decomposition).
- **T** = S+A+R = total frames from move start to first-actionable.
- **Hitstop (`frz`)**: not applicable on whiff captures (`frz=0`
  throughout). On HIT/BLOCK captures, frames with `frz > 0` are excluded
  from the A/R tally before applying the same rule (validated once, see
  the yang-forward deviation below — not separately validated beyond
  that one internal-consistency check).

This convention was derived from, then validated against, this repo's
own `tools/frame-data/golden/*.tsv` PASS rows (below) before being
trusted on any disputed row.

## Instrument validation (8/8 exact)

Before touching any disputed row, the rig's counting convention was
checked against rows this repo's engine already measures arcade-exact.
The three Ryu rows below are session 1's original controls, each a
**single-run capture** (`cap-ryu-{lp,mp,lk}.log`, not repeated). The
five session-2 rows (Sean/Urien/Yang/Ibuki/Q far LP) are each a repeat
rep against a savestate anchor (2x, byte-identical logs, `diff`
verified):

| Control | Golden (PASS) S/A/R | Captured arcade S/A/R | Result |
| --- | --- | --- | --- |
| Ryu LP whiff | 4/3/4 | 4/3/4 | EXACT |
| Ryu MP whiff | 5/4/9 | 5/4/9 | EXACT |
| Ryu LK whiff | 4/4/7 | 4/4/7 | EXACT |
| Sean far LP whiff | 3/2/5 | 3/2/5 | EXACT |
| Urien far LP whiff | 4/2/6 | 4/2/6 | EXACT |
| Yang far LP whiff | 3/2/6 | 3/2/6 | EXACT |
| Ibuki far LP whiff | 2/2/2 | 2/2/2 | EXACT |
| Q far LP whiff | 6/4/4 | 6/4/4 | EXACT |

8/8 exact across two independent sessions — the instrument and
convention are validated before being applied to any disputed row.

## Disputed-row results (session 2 sweep)

Attacker = P2 (right side, facing left); P1 = idle dummy (Alex). All
captures 2 reps, byte-identical. Character identity verified in-memory
via the char-id word (sean=12, urien=13, yang=10, ibuki=7, q=18).

| Move | Oracle (JSON) | This engine | Arcade (captured) | Verdict |
| --- | --- | --- | --- | --- |
| Sean Dragon Smash Jab (dp+LP), whiff | 5/6/36 | 5/6/36 | 5/6/36 | ARCADE==ORACLE==ENGINE |
| **Sean Dragon Smash Strong (dp+MP), WHIFF-only capture** | 5/6/36 (Jab-duplicate) | 7/8/39 | **7/8/39** | **ARCADE==ENGINE; oracle row confirmed a Jab-duplicate data-entry error** |
| Sean Dragon Smash Fierce (dp+HP), whiff | 9/9/42 | 9/9/42 | 9/9/42 | ARCADE==ORACLE==ENGINE |
| Urien Universal Overhead (MP+MK), whiff, active frames | 10 | 9 (measured) / 11 (played) | **10** | **ARCADE==ORACLE** — decides G3: neither engine quantity (9 or 11) matches; arcade sits strictly between them |
| Urien Universal Overhead, whiff R | 5 | 3 | **5** | **ARCADE==ORACLE** |
| Yang Senkyuutai LK (qcf+LK), whiff R | 34 | 38 | **34** | **ARCADE==ORACLE** — engine's +4 is engine-side |
| Yang Forward (close MK), S (box-appear vs. first-connect) | S=7 | first_active_raw=5 / arm tick=7 | box appears tick 6, first connects tick 8 | **BOTH real on hardware** — box exists 2 ticks (6,7) before it can connect at tick 8; confirms the ENGINE-6 census conclusion directly on real hardware |
| Ibuki Kazekiri LK (F,D,DF,F+LK), whiff R | 26 | 29 | **26** | **ARCADE==ORACLE** |
| Q Universal Overhead (MP+MK), whiff R | 5 | 3 | **5** | **ARCADE==ORACLE**; box-A=10 both sides |

Only the WHIFF leg of Sean Dragon Smash Strong (dp+MP, dist 321) was
captured on real hardware. The corpus's block/hit entries
(`sean-dragonsmash-mp-block`/`-hit`) convert to the same S=7/A=8/R=39
value on the strength of this move's own leg-uniformity — Jab and
Fierce both already measure identically on WHIFF/BLOCK/HIT in this
repo, and Strong's own pre-capture measurement showed the same
uniformity before the arcade capture confirmed the WHIFF number — not a
separate arcade capture of the block or hit leg. `adv=-30` on the BLOCK
leg was never arcade-captured at all (the rig's frame log has no `adv`
field); it is the same real recovery value, arithmetically derived, per
`tools/frame-data/corpus-sean.yaml`'s `sean-dragonsmash-mp-block`
comment.

Full per-row frame citations (f1/first-box/busy-transition frame
counters) are in the session sweep record; see Reproduction below to
regenerate them.

### Reading the Urien UOH result (PORT-DIVERGENCE-1)

Every sampled item-18/ENGINE-5 row above (Urien UOH A and R, Yang
Senkyuutai R, Ibuki Kazekiri R, Q UOH R) came back **arcade == oracle**.
The disputed tables were right all along; the divergence is entirely on
this engine's side. For Urien UOH specifically the engine plays a total
of measured-9 + a distinct played-11 + a declared-12 (see
`docs/frame-data-synthesis.md` §12.2.4's "Same-cgix non-sentinel
self-loop" row) against arcade's clean 10 active + 5 recovery = 15
frames from first box to busy-clear — none of this engine's own
reachable quantities equal arcade's. This finding is registered as
**PORT-DIVERGENCE-1** (see the synthesis doc's item-18 register update
and `docs/plan-frame-data-completion.md`'s tracker) — the layer at which
the divergence actually lives (the arcade-to-Dreamcast/PC port this
engine is decompiled from, vs. this repo's own overlay/meter
accounting, vs. a possible ROM-revision mismatch — see Caveats below)
is explicitly **unknown** and not chased further by this capture; a
possible future bounded probe (whether the engine's 11th box frame is
hit-check-eligible) is flagged, not pursued.

**DATED UPDATE (2026-07-11, LAYER-1).** PORT-DIVERGENCE-1 above is
RETRACTED -- see `docs/frame-data-synthesis.md`'s new §13.16 for the
full triangulation. A convention-twin engine-raw probe (env-gated
scratch instrument, never committed, 6/6+1 validation, both windows
md5-proven applied/reverted) measures this engine's raw frames on
Urien UOH whiff at S15/A10/R5 -- identical to arcade on both revisions.
The layer is (a) OVERLAY/METER, not an unresolved port-vs-arcade
question: this engine's raw box-active timeline plays exactly what
arcade plays; only the overlay's credit-based A accounting (9/11/12)
and its R-endpoint construction diverge from that raw timeline.

Separately, a dated correction to this section's own prose above: the
phrase "the arcade-to-Dreamcast/PC port this engine is decompiled
from" misstates this repo's decomp base. This repo's own `README.md`
is unambiguous -- "Based on a decompilation of the PlayStation 2 port"
-- and the upstream decomp project's own build config
(`crowded-street/3s-decomp`'s `docs/builde-guide.md` +
`config/anniversary/sfiii.anniversary.yaml`) names the exact target:
`THIRD_U.BIN`, the US-region executable of the PS2 *Street Fighter
Anniversary Collection* disc. No Dreamcast or PC decomp base is
corroborated anywhere in this repo. Original phrasing preserved above,
uncorrected in place, per this doc's append-only discipline -- this is
the dated correction.

## Session 3 (sfiii3nr1, Japan 990512 NO CD)

**Status: rig re-run against the REFERENCE revision, 2026-07-11
(LAYER-1).** Session 3 re-ran the identical bridge/rig methodology (Rig,
Control channel, Frame-counting convention above, unchanged) against a
second ROM, `sfiii3nr1.zip` -- Street Fighter III 3rd Strike: Fight for
the Future, Japan 990512 NO CD. Boot title proof: the emulator window
title read back after launch as "Street Fighter III 3rd Strike: Fight
for the Future (Japan 990512, NO CD)", confirming the correct
romset/driver was accepted.

**Instrument gate -- PASSED 3/3, then 13 control rows total.** The
three Ryu controls (LP/MP/LK whiff, same as Session 1) were re-run
first and matched exactly: 4/3/4, 5/4/9, 4/4/7. Once gated, a
10-character control record (Ryu plus Remy/Oro/Yun/Akuma far-LP whiffs
as new-character controls, plus Sean/Yang/Ibuki/Urien/Q far-LP whiffs
as B2 representative controls) reconfirmed the rig's counting
convention on the new ROM -- 13 control rows total, all EXACT, before
any disputed row was touched.

**B1 -- new disputed-row captures (990512), 2 reps byte-identical
each:**

| Move (recipe) | Oracle | This engine (golden) | Arcade 990512 | Agreement |
| --- | --- | --- | --- | --- |
| Remy Cold Blue Kick (qcb+LK), whiff | 17/10/10 | 17/11/9 (XFAIL) | **17/10/10** | == ORACLE (A/R boundary sits at 10/10 on hardware) |
| Oro Oniyama (charge-down 60, then-up LP), whiff | S6/A10, R magnitude 29 (oro.json/FAT-3S negative; EventHubs positive) | 6/10/28 (R masked, MASK-1) | **6/10/29** | S/A == both; R=29 vs engine 28; magnitude matches oracle exactly |
| Akuma UOH (MP+MK), whiff | 15/8/7 | 15/8/6 (XFAIL R) | **15/8/7** | == ORACLE |
| Oro UOH (MP+MK), whiff | 15/10/5 | 15/10/4 (XFAIL R) | **15/10/5** | == ORACLE |
| Yun UOH (MP+MK), whiff | 15/9/6 | 15/9/10 (XFAIL R) | **15/9/6** | == ORACLE (arcade sits at oracle 6, not engine 10) |
| Remy UOH (MP+MK), whiff | 15/10/5 | 15/11/10 (XFAIL) | **15/10/5** | == ORACLE (both A and R) |

Raw fc citations (r1; r2 byte-identical): remy-cbk f1=269689, box
269706-269715, busy768=269726 (T=37); remy-uoh f1=269683, box
269698-269707, busy768=269713; oro-oniyama f1=277334, box
277340-277349, busy768=277379 (T=45; pre-move charge frames
act=00000007/busy=0 correctly excluded); oro-uoh f1=277274, box
277289-277298, busy768=277304; yun-uoh f1=284653, box 284668-284676,
busy768=284683; akuma-uoh f1=290403, box 290418-290425,
busy768=290433.

**B2 -- re-validation of all seven 990608 rows against 990512, 2 reps
byte-identical each:** Sean Dragon Smash trio (5/6/36 Jab, 7/8/39
Strong, 9/9/42 Fierce), Urien UOH (15/10/5), Yang Senkyuutai LK (R=34),
Ibuki Kazekiri LK (R=26), Q UOH (R=5, box-A=10 both sides) --
**every row byte-reproduces on 990512.** Raw fc citations (r1): sean-ds
f1=305822, boxes lp 305827-305832 / mp 305829-305836 / hp
305831-305839, busy768 305869/305876/305882; urien-uoh f1=311498, box
311513-311522, busy768=311528; yang-senk f1=296741, box 296749-296756,
busy768=296791; ibuki-kaze f1=323004, box 323008-323019,
busy768=323046; q-uoh f1=328980, box 328995-329004, busy768=329010.
Revision explanations are ruled out for every re-validated row/clause;
no re-anchor fires on any of the seven.

**Oniyama recipe correction.** The move's recipe is the corpus's own
`motion charge-down 60 then-up LP` (`tools/frame-data/corpus-oro.yaml:722`),
not the "dp+LP" prose this document's Follow-up targets section used
below -- act id 00050020 plus S/A matching both oracle and engine
exactly confirms the right move was captured.

**Session deviations (session 3, brief).** (1) A ~2h screen-lock
blocker: the console session was locked when the lane started, and
`fcadefbneo.exe` crashed at startup (Wine exit c0000005) on every
launch attempt under the locked WindowServer, including a control
launch of the already-proven Session-1/2 recipe -- isolating the cause
as environmental (screen lock), not romset/driver; bounded polling
caught the unlock and the very next launch booted clean. (2) Host
`screencapture` was TCC-blocked this session; emulator-side screenshots
(`gui.gdscreenshot()` written in-process, converted by new helpers)
replaced it -- no host-side input was ever synthesized (the Session-1
hard ban held). (3) A shadow savestate symlink
(`fbneo-workdir/savestates/sfiii3nr1_fbneo.fs`, itself a symlink into
the app's stock savestates) was removed as a crash-cause hypothesis
before the lock-screen cause was identified; removal touched only the
shadow dir, not the real install, and was not restored (990512 boots
fine without it).

### Reference revision (2026-07-11, LAYER-1)

`sfiii3nr1` / Japan-990512-NO-CD is this program's REFERENCE revision
going forward. Grounds: (i) competitive standard -- 990512 is the
tournament/competitive-standard revision (Fightcade ranked runs it);
(ii) upstream anchor -- this repo's own `src/arcade/
arcade_char_data.c:421-422` reads `sfiii3nr1.zip` verbatim, the only
arcade ROM this repo's code ever reads; (iii) now measured identical to
Euro-990608 on all 7 re-validated rows plus 13 controls (above) -- the
dual-revision agreement is real, not a coincidence of the sample.

## Session 4 (sfiii3nr1, Japan 990512 NO CD) — RE-ANCHOR-1 Gate A

**Status: 2026-07-11, RE-ANCHOR-1 program (`docs/frame-data-synthesis.md`
§13.17).** Rig identical to Session 3. Instrument gate (ryu far LP whiff
4/3/4, EXACT) plus 5 per-character controls (urien/chunli/dudley/ibuki
far LP whiffs, all EXACT), 2 reps byte-identical throughout.

**Target 1 — Urien Headbutt WHIFF recovery, per strength vs its OWN
oracle** (`urien.json`: Jab=19, Strong=18, Fierce=19 — never a uniform
19): arcade 990512 measures S/A exact on all three (7/2, 9/4, 12/6) and
busy-edge R = **16/15/16** — uniformly 3 below each strength's own
oracle. Raw fc: lp f1=251031, box 251038-251039, busy768=251056; mp box
251040-251043, busy768=251059; hp box 251043-251048, busy768=251065.

**Target 2 — UOH whiff box-A (PENDING-USER-1 hardware backing)**: arcade
990512 measures S/A/R = 15/10/5 on chunli-uoh, dudley-uoh, ibuki-uoh
(all three previously untested on hardware) — box-A=10 on all three,
confirming the raw-box-count amendment's figure independently of the
prior q-uoh-only hardware backing (Sessions 2-3). Raw fc: chunli
f1=256373, box 256388-256397, busy768=256403; dudley f1=261601, box
261616-261625, busy768=261631; ibuki f1=266617, box 266632-266641,
busy768=266647.

Full raw logs/derivation and session deviations: `<sp>/capture/
session4-results.md`, `s4-derive-all.txt`, `cap-s4-*.log` (this
capture's raw artifacts are scratchpad-only, not committed to this
repo — see "Session index" below).

## Session 5 (sfiii3nr1, Japan 990512 NO CD) — lever-N actionability probe

**Status: 2026-07-11, RE-ANCHOR-1 ladder step
(`<sp>/reanchor/leverN-rediagnosis.md` §3, the gate spec executed
verbatim).** Session 4's busy-edge reads on the Urien Headbutt trio, and
the RE-ANCHOR-1 census's two counterexamples (`ibuki-twdsforward-whiff`,
`ibuki-twdsroundhouse-whiff` — currently-PASS arcade-exact windows whose
lever-N-predicted R diverges from golden), share one signature: oracle ==
overlay-natural-end == busy-edge+3, a 3-tick post-edge `rno3==3` residue.
Session 4's busy-edge convention cannot by itself tell whether that
residue is actionable (the convention IS the busy edge, by construction)
— only a direct actionability probe can.

**Method:** instrument gate (ryu far LP 4/3/4, ibuki far LP 2/2/2, urien
far LP 4/2/6, all EXACT) then, per target row, TWO measurements: (a) the
busy-edge read (unchanged convention, confirms Session 4's values with
NO divergence), (b) an actionability probe — from 2 ticks before the
busy edge, hold neutral, alternate LP/MP presses every frame for 40
frames, and record `first_new_act_fc`, the first frame a NEW act id with
`rflag!=0` appears (a punch/kick pair avoids 3S's throw-input leniency
that an LP/LK alternation triggered on the first attempt, redesigned
mid-session, both variants' logs retained). A validity control
(`ibuki-kazekiri-lk-whiff`, an already arcade-exact busy-edge PASS row
with its own post-edge tail) confirms the probe measures actionability,
not routine end, by accepting mid-tail exactly as its own oracle
predicts.

**Result — OUTCOME A on every probed row:** `ibuki-twdsforward-whiff`,
`ibuki-twdsroundhouse-whiff`, and all three Urien Headbutt strengths
accept a new input exactly at `busy_edge + 0` (calibrated against the
`ibuki-twdsshort-whiff` control, whose own offset `o = 0`) — never at
`busy_edge + 3`. No press before the edge is ever accepted on any row.
The busy edge IS each row's first-actionable frame; the oracle's larger
figure (4/21/19/18/19 respectively) counts a 3-frame non-actionable-
claimed residue that is in fact actionable.

Full raw logs, probe analyzer, and session deviations (a ~17-minute
screen-lock blocker, resolved within the session's own bounded-polling
allowance; the probe-mash redesign): `<sp>/capture/session5-results.md`,
`s5-derive-all.txt`, `s5-probe.py`, `cap-s5-*.log` (scratchpad-only, not
committed to this repo).

### `derive2.py` adoption note (Session 6 onward)

Starting with Session 6 (`<sp>/zero/arcade-track-a/`, base Headbutt
family + jinchu targets), every session's raw-log deriver is `derive2.py`,
a corrected copy of the original `derive.py` used through Session 5.
**Fix:** the original deriver anchored a move's Startup boundary (`S`) on
the first `frz`-excluded active frame — but CAPTURE.md's own literal
convention only excludes `frz>0` frames from the A/R tallies, not from
the S boundary itself. On any capture whose freeze begins ON the box's
own opening frame (the common contact-leg shape), the old deriver
silently shifted frames out of S and into a phantom gap, undercounting S
by however many ticks froze before the first *unfrozen* active frame.
`derive2.py` anchors S on the RAW first `box>0` frame (freeze included),
while A/R still exclude `frz>0` frames per the pre-existing Yang-Forward
HIT precedent — a strictly more correct reading of CAPTURE.md's own
documented convention, not a convention change. Every session from 6
onward (`<sp>/zero/arcade-track-a/`, `<sp>/zero/arcade-s7/` through
`<sp>/zero/arcade-s11/`) uses `derive2.py`, each a verbatim copy of the
prior session's (session 7's own copy of session 6's; sessions 8-11 each
copied the immediately-prior session's file unmodified) — no further
corrections were needed after the Session 6 fix. `derive2.py` also
reports `raw_box_frames`/`active_runs`, a signature this program's own
UOH-CLOSURE work (Session 11) later relied on to distinguish a genuine
block capture from a whiff-shaped one (raw box length matching a
character's own whiff-canonical active count, vs freeze-extended on a
real contact) — see Session 11 below.

## Session 6 (sfiii3nr1, Japan 990512 NO CD) — TRACK-A batch + Urien Headbutt contact-leg extension

**Status: 2026-07-13, feeds CONTACT-2's own Urien Headbutt contact-leg
citation (`docs/frame-data-synthesis.md` §13.19) plus several still-open
TRACK-A items.** Captured all 10 TRACK-A rows (Oro Jinchu Nobori LK/EX,
4 legs; Twelve SA1 X.N.D.L. block/hit; Twelve SA3 X.C.O.P.Y. activation;
Urien SA2 Temporal Thunder block/hit swept across 3 distances; Urien SA3
Aegis Reflector activation) plus a coordinator-added extension (Urien
Headbutt LP/MP/HP, BLOCK+HIT, 6 rows). Found and fixed a convention bug
in the inherited `derive.py` (S incorrectly anchored on a `frz`-filtered
first-box frame instead of the raw one — see the `derive2.py` adoption
note above, which this session originated).

Two genuinely new engine-bug findings, neither a clean oracle-vs-engine
flip: Oro Jinchu Nobori's contact legs DO reach a real busy 771→768 edge
on real hardware (R=33 on the base LK version, 34 HIT/52 BLOCK on the EX
version — an 18-frame block/hit asymmetry the base move doesn't share)
that the ENGINE-8 investigation's own internal state never reaches at
all, falsifying that investigation's "no-code-fix-exists" verdict —
though the correct fix target is arcade's own value, not oracle's
uniform 19 (routed to TRACK-E, not closed by this session). Urien
Temporal Thunder shows a flat ~144-145-frame busy-edge span across a 6x
range of distances (52-318px), directly contradicting the engine's own
claimed 11-to-83 A-scaling (`box` reads 0 throughout — a beam/lightning
super with no `att_box` tensor use, so this rig cannot independently
derive the disputed A itself). Two install-type supers (Twelve
X.C.O.P.Y., Urien Aegis Reflector) both show real actionable-return
times, actionability-probe-confirmed, 5-7x larger than either their
engine or oracle Startup figures (~95-96 and 65 frames respectively) — a
repeating pattern (joining Session 7's own install-family findings, see
below) suggesting install/activation supers as a class need dedicated
re-derivation. Twelve X.N.D.L. stays inconclusive: confirmed proj-split
with zero `att_box` visibility on real hardware (matching the engine's
own finding), so this rig cannot independently derive its disputed
Startup without a new projectile-object instrument.

Separately, the **Urien Headbutt contact-leg extension** (LP/MP/HP ×
BLOCK/HIT, 6 rows) is the result CONTACT-2 (§13.19) and this document's
own `derive2.py` adoption note cite directly: every contact leg cleanly
reproduces the already-shipped WHIFF busy-edge values (16/15/16 per
strength, `docs/arcade-frame-data/ERRATA.md` §6) exactly, per strength,
on both BLOCK and HIT — confirming this move's own raw counting is
outcome-independent and closing 6 rows of the "contact-leg
raw-box/busy-edge meter extension" bucket for Headbutt specifically (S/A
pairs also matched each strength's own oracle exactly, re-confirming
Session 4/5's WHIFF-leg finding on the contact legs).

All raw per-frame tapes, `derive2.py` (this session's own corrected
deriver, see the adoption note above), and screenshots are preserved in
`<sp>/zero/arcade-track-a/` (scratchpad-only, not committed to the
repo).

## Session 7 (sfiii3nr1, Japan 990512 NO CD) — UOH contact-leg settlement + install-family discriminator

**Status: 2026-07-13, UOH-CLOSURE lever T evidence base
(`docs/frame-data-synthesis.md` §13.20).** Captured Akuma/Yun/Ibuki UOH
(`press MP+MK`, "Universal Overhead") BLOCK+HIT (6 rows, 2 reps
byte-identical each) plus actionability probes on all 3 BLOCK legs,
settling whether the busy-edge/lever-R construction (§13.19) also covers
this move class on these three characters: **busy-edge matches real
arcade on 6/6 rows tested** — Akuma block 8/hit 6, Yun block 8/hit 6,
Ibuki block 7/hit 5 (S/A unaffected). This dissolves an apparent "hard
counterexample" from the offline design fit (Akuma/Yun both reading
tail=8 but seemingly "needing" different per-character values 7/6): both
actually need the SAME value (8) once measured on real hardware. The
published oracle is stale on 4 of the 6 rows (both Akuma legs,
Yun-block, Ibuki-block), including 2 rows (both Akuma legs) that were
shipping as PASS at the time — a second confirmed "wrong-data-passing"
instance, same shape as the Urien Headbutt precedent (Session 3/4).
Separately, ran an install-family discriminator: actionability probes on
Yun Genei Jin, Q Total Destruction, and Yang Sei-ei Enbu (SA3
activations) all show real actionable-return times 3.9x-8.2x their
oracle Startup value, joining Session 6's Twelve/Urien results for a
clean 5/5 — the whole install-super family's oracle Startup field is a
narrow (flash-only) quantity, not a per-character anomaly.

Rig setup this session (new since Session 5): a `hold_for()` input bug
(P1/P2 taps landing as silent no-ops under a mis-registered bridge hook,
caught before any real capture) and a fresh ~51-minute screen-lock
c0000005 crash (bounded polling, same class as Sessions 3/4/5). Raw
tapes, `derive2.py` (session 6's corrected deriver, reused verbatim), and
navigation screenshots preserved in `<sp>/zero/arcade-s7/`
(scratchpad-only, not committed).

## Session 8 (sfiii3nr1, Japan 990512 NO CD) — the six-character BLOCK/HIT sweep (ryu/ken/oro/sean/q/chunli)

**Status: 2026-07-13, UOH-CLOSURE evidence base — SIX BLOCK CAPTURES
RE-ADJUDICATED, see the dated correction below.** Captured
ryu/ken/oro/sean/q-samef/chunli UOH BLOCK legs (6/6, byte-identical x2,
all probe-confirmed) and HIT legs (6/6, byte-identical x2; probes not
run this session). At capture time this session's own BLOCK results
appeared to be a genuine mixed population — 5 of 6 (ryu/ken/oro/sean/q)
read their currently-shipped LEGACY display exactly, disagreeing with the
busy-edge construction, while Chun-Li's read 5 (matching neither computed
value) — the one apparent "hardware sides with legacy" result and the
one apparent genuine divergence in this whole program. The HIT legs, by
contrast, cleanly matched the busy-edge construction on all 6
(ryu/ken/oro/sean/q-samef +1 each over the same characters' own HIT
legacy value; chunli-hit 4, already valid).

**DATED CORRECTION (2026-07-13, UOH-CLOSURE §2.2, `docs/
frame-data-synthesis.md` §13.20).** Re-deriving the freeze/box overlap
directly from this session's own raw tapes (not the session's original
summary) found the six BLOCK captures above carry a complete
**whiff signature**, not a block signature: (1) each capture's raw box
window length equals that character's own whiff-canonical active count
exactly (ryu 9, ken 8, oro 10, sean 8, q 10, chunli 10), where every
verified-contact leg's box window is freeze-extended well beyond it; (2)
zero attacker OR defender freeze anywhere in any of the six windows,
where a real guarded UOH freezes the attacker 8 ticks on every character
per the engine's own shared hit-check path (LAYER-1-confirmed faithful);
(3) the wall-clock span from first-active to the busy edge equals the
whiff-canonical S+A+R sum on all six (30), vs 41 on a real block. The
likely cause: these six BLOCK captures held `P1 Left` from a 50-56px
anchor gap for the whole window, walking the guard-stance dummy backward
through the 15-tick startup until these six characters' short-reach UOHs
missed the retreated hurtbox — their own HIT legs (P1 fully idle, no
walkback) connected at the same anchor. **The six voided BLOCK captures
were superseded by Session 11's re-capture** (dummy cornered, walkback
structurally impossible, mandatory in-tape contact witness) — see
Session 11 below; this session's own HIT legs and the install-family
Akuma/Yun/Ibuki results (Session 7) are unaffected, still valid-contact
tapes (`frz=8` in-box, `p1life` 160->155).

Raw tapes and `derive2.py` (Session 7's own copy, reused verbatim)
preserved in `<sp>/zero/arcade-s8/` (scratchpad-only, not committed).

## Session 9 (sfiii3nr1, Japan 990512 NO CD) — XFAIL-18 bucket family closure (alex/dudley/elena/necro/twelve/urien/remy/yang)

**Status: 2026-07-13, UOH-CLOSURE evidence base
(`docs/frame-data-synthesis.md` §13.20).** Captured the remaining 8
characters of the 9-character "XFAIL-18 bucket" (Session 7 already
captured Yun's pair) — all 16 BLOCK+HIT legs, 2 reps byte-identical each,
BLOCK legs probe-confirmed on all 8 (new act accepted exactly at the
busy edge, zero early acceptance), HIT legs probe-confirmed on 3/8
(dudley/elena/yang). **Result: arcade R equals the busy-edge/lever
construction on all 16 of 16 rows, zero exceptions** — 7 of the 8
characters read a uniform BLOCK R=4 / HIT R=4 (differing only in their
own currently-shipped golden A, which ranges 9-15 across the 7 despite
arcade A being a fixed 14/10 pair on all 7); Yang is the one structural
outlier, BLOCK R=10 / HIT R=8, matching Yang's own already-known-outlier
lever prediction. Combined with Session 7's Yun pair, the whole 18-row
XFAIL-18 bucket is now 18/18 measured on real hardware, 100% siding with
the busy-edge construction and 0% matching either the whiff-canonical
oracle (R=5, or 6 for Yun) or the currently-shipped golden XFAIL figure.

Raw tapes and `derive2.py` (reused verbatim) preserved in
`<sp>/zero/arcade-s9/` (scratchpad-only, not committed).

## Session 10 (sfiii3nr1, Japan 990512 NO CD) — Urien EX Headbutt contact-leg capture (CONTACT-2 §8.3.7 blocker)

**Status: 2026-07-13, resolves CONTACT-2's own §8.3.7 pre-registered
conditional** (already cited by CONTACT-2's shipped payload,
`docs/frame-data-synthesis.md` §13.19 — this is that entry). Captured
`urien-exheadbutt-block`/`-hit` (2 reps byte-identical each, both
actionability-probe-confirmed) plus a fresh whiff identity check (2 reps
byte-identical) confirming EX-not-base via act id (`0005002C` vs base's
`00050029`/`2A`/`2B`) and the EX-specific S/A pair (9/4). **Result: both
contact legs measure R=12 on real hardware — Branch A**, matching the
busy-edge/lever's predicted value and diverging from the currently-shipped
oracle-exact R=15 by the same -3 magnitude already established for the
base Headbutt family (Session 6). This also newly confirmed the WHIFF
leg's own R=12 on real hardware for the first time (previously shipped
"by analogy" only). No c0000005 screen-lock crash this session (first
launch booted clean); the `gui.gdscreenshot()`+`gd2png.py` screenshot
path was rediscovered fresh (Lua state and helper functions do not
persist across an emulator relaunch).

All raw tapes, `derive2.py` (copied verbatim from Session 9, unmodified),
and navigation screenshots preserved in `<sp>/zero/arcade-s10/`
(scratchpad-only, not committed to the repo).

## Session 11 (sfiii3nr1, Japan 990512 NO CD) — UOH BLOCK-leg re-capture with mandatory contact witness

**Status: 2026-07-13, resolves the UOH-CLOSURE lever-T payload blocker
(`docs/frame-data-synthesis.md` §13.20).** Re-captured
`ryu/ken/oro/sean/q-samef/chunli` UOH BLOCK legs (each corrected against
Session 8's proven walkback-whiff contamination by cornering the dummy
first) plus a directly-measured `q-uoh-chain-retrigger` capture
(previously only a by-extension prediction), each with a whiff identity
check (6/6 exact vs oracle), 2 byte-identical reps, and an actionability
probe (7/7 exact at the plain trace's own busy-edge fc, zero early
acceptance) — every leg additionally carries an explicit in-tape contact
witness (attacker `frz` 8->0 overlapping the box-active window, box
freeze-extended well beyond the character's own whiff-canonical active
count), the discriminator Session 8's voided captures lacked entirely.
**Result: BRANCH A on all 7 legs** — arcade R equals the busy-edge/lever-T
construction on every leg (ryu 7, ken 8, oro 6, sean 8, q 6,
chain-retrigger 6, chunli 4), diverging from the legacy shipped values
(6/7/5/7/5/5) by exactly +1 on the five shoto/Oro/Sean/Q legs and landing
display-invariant on Chun-Li (4=4, NOT the 5-exception Session 8's voided
tape suggested). No Branch-B counterexample on any leg — the UOH-CLOSURE
payload is unblocked. Methodological findings this session (an
input-hook naming bug, a savestate-anchor race, a numbered-slot savestate
pitfall, a DSL wait-offset miscount, and a derive-before-flush race) were
each caught by this session's own rep-vs-rep `diff`/`wc -l` discipline
before being able to corrupt a cited result — none of the seven final
cited legs rest on an affected capture.

All raw tapes, `derive2.py` (copy of Session 10's own deriver, verbatim,
not modified), `bsend.sh`, and navigation screenshots preserved in
`<sp>/zero/arcade-s11/` (scratchpad-only, not committed to the repo).

## Session 12 (sfiii3nr1, Japan 990512 NO CD) — first contact-leg ACTIVE-frame (A) ground truth

**Status: 2026-07-14, contact-leg A ground truth (`docs/frame-data-synthesis.md`
§13.16 CONTACT-A addendum).** Captured the 9 grant-route contact-A-overcount
legs (ibuki SA2 Yoroi Doushi/Chi-Blast, sean EX Dragon Smash, chunli SA2
Houyoku Sen, remy Cold Blue Kick LK+EX), 2 reps byte-identical each,
whiff-identity-gated (raw box == corpus `box_a` on all four reachable whiffs),
contact-witnessed (attacker hitstop overlapping the box-active window +
chip-vs-full `dlife` split distinguishing BLOCK from HIT), dummy cornered
against the stage wall (walkback structurally impossible). **Result: on every
reproducible contact leg, arcade active frames (this document's own
frz-EXCLUDED convention: box>0 AND frz==0) == ORACLE** — 8 of 9 exactly;
the lone exception, `chunli-sa2-hit`, reads oracle−1 (a genuine block/hit
multi-hit boundary: the launcher's final active box stays 1 frame longer on
block than on hit, verified in the raw tail, not an instrument error). **The
engine/overlay "measured" (golden) value OVERcounts the oracle on every
leg** — +1 for a single hitstop-boundary tick (sean, chunli-block,
ibuki-hit: the contact frame that starts hitstop is itself box-active and is
counted by the overlay, excluded by the frz-rule) or +4/+2 for Remy Cold Blue
Kick LK/EX (declared-credit banking, the §13.13/F13 same-tick
interior-transition re-credit mechanism the corpus ledger walk already
predicted) — **arcade never sides with the measured/overlay value on any
leg.** The literal "(measured−oracle)==in-box-freeze-count" hypothesis is
REFUTED (the overlay does not naively count all raw box frames — if it did,
sean would read 32 and remy-cbk-lk 22, not 13/14); the CORE claim
"arcade contact-A == oracle, measured is a meter artifact" is CONFIRMED —
the first hardware proof of the contact-leg-A layer LAYER-1 (§13.16) flagged
as untrustworthy-by-instrument. Convert-to-measured: 0 of 9, confirming the
grant-route "0/9" adjudication (`<sp>/zero/grant-route/adjudication.md`) on
primary hardware evidence. A freeze-filtered contact-A re-anchor is VALIDATED
(reproduces oracle exactly) for the 6 single-hit special/EX rows
(sean-exdragonsmash ×2, remy-cbk-lk ×2, remy-cbk-ex ×2); QUALIFIED for the
rapid multi-hit SA (`chunli-sa2-block` needs a multi-hit-boundary-aware rule
to avoid regressing the currently-PASS `chunli-sa2-hit`); N/A for the grab
(`ibuki-sa2-hit` — the raw `att_box` instrument populates for 64 frames vs
the overlay's `box_a`=16, breaking the "raw box == overlay box_a" identity,
though the frz-excluded active count still == oracle=15) and for
`ibuki-sa2-chiblast-block`, which is **NOT arcade-reproducible**: Yoroi
Doushi's command grab could not be denied by any manual guard-hold (stand or
crouch, tested 2× each) — the harness's "dummy: stand" is training-mode
"ALL GUARD" throw-immunity (`src/test/input_script.c:130-153`), a state a
real opponent's guard cannot reach, so the row's measured A=16 describes a
training-mode-only scenario with no normal-play arcade quantity to confirm
or refute it. (Whether any of these 6 validated single-hit rows or the
chunli/ibuki qualifications actually ship as an engine lever is a SEPARATE,
already-decided question — see `docs/frame-data-synthesis.md`'s CONTACT-A
addendum: a full-universe census proved the freeze-filtered construction
engine-signal-indistinguishable from 113 other currently-PASS contact-A
rows it would regress, so no lever ships despite this session's positive
hardware confirmation.)

**New rig capabilities, reusable by future sessions:**
- **`character_select_id` memory override (P2 base+0x3C8 select-id byte,
  `0x02011388`).** Writing this byte every frame while P2's select state is
  < 5 FORCES P2 to spawn as that character regardless of cursor position
  (in-match `char_id` at base+0x3C0 read back exactly: ibuki=7, sean=12,
  chunli=16, remy=20 — all confirmed) — eliminates grid navigation
  entirely; the cursor can sit anywhere (Ryu by default) and the locked-in
  character is whatever id was forced. The SA-slot index still transfers to
  the forced character (verified: forcing Ibuki + picking SA-slot 1 =
  Ibuki's own SA2 Yoroi Doushi).
- **Round-timer freeze (`0x02011377`, write 100 every frame = "infinite
  time").** Prevents the 99s in-match round timer from expiring mid-setup
  and dumping to the CONTINUE screen during long capture-rig setup
  sequences — combined with the existing select-timer freeze
  (`0x020154FB`, write `0x30` every frame, Session 2), setup time is now
  effectively unbounded on both the select screen and in-match.

All raw tapes (24, field-line `fc= act= rflag= busy= frz= box= x= |
dfrz= dbusy= dbox= dlife=`), `derive2.py` (verbatim copy of Session 11's
own deriver) + secondary independent counter `analyze.py`, sender
`bsend.sh`, helper install `helpers.lua`, and navigation screenshots
preserved in `<sp>/zero/arcade-s12/` (scratchpad-only, not committed to
the repo).

## Session 13 / CAP-3 (sfiii3nr1, Japan 990512 NO CD) — projectile-object visibility instrument + disputed projectile-super captures

**Status: 2026-07-14 (`<sp>/zero/cap3/instrument-report.md` Phase-1 +
`phase2-report.md` Phase-2; `docs/frame-data-synthesis.md` §13.16
ORACLE-TABLE-INTERNAL-INCONSISTENCY addendum). FLAG FOR FABLE RE-REVIEW.**

The prior rig read only the two PLAYER structs; for a fireball the attacker's
own attack box stays 0 for the whole move (the attack lives on a separate
spawned "tama" object), so the disputed projectile-super rows could not be
measured. CAP-3 built and validated a projectile-object instrument. Phase-1
validated it against Ryu Hadouken (spawn slot 10 / R=36 / T=46, byte-exact vs
corpus on all three strengths, full spawn→active→contact→despawn lifecycle
with a working defender contact-witness). Phase-2 then captured all five
disputed projectile targets, 2 reps byte-identical each — **the instrument
SEES every disputed super's projectile attack box; ZERO capability-gaps
remain.**

**PERMANENT RIG-CAPABILITY REFERENCE — projectile-object memory map.** Source
of truth: grouflon `3rd_training_lua/src/gamestate.lua:972-1055`
(`read_projectiles`) + `:221-250` (`read_game_object`), the same map the rig
already trusts for the two player structs. The projectile pool is a separate
object array, distinct from the player structs, walked as a linked list:

| Quantity | Address / formula | Notes |
|---|---|---|
| Object array base | `0x02028990` | pool of spawned objects, 0x800 (2048)-byte stride |
| Object index table | `0x02068A96` | per-list head-index words |
| Projectile list index | `3` | `head = readwordsigned(0x02068A9C)` (= `0x02068A96 + 3*2`) |
| Object base from index | `0x02028990 + (idx << 11)` | `idx << 11` = `idx * 2048` |
| Linked-list "next" | `readwordsigned(base + 0x1C)` | terminate on `-1` (0xFFFF) or 30 objects |

Per-object fields (relative to the object base — same struct layout as a
player, so the existing object-generic `attack_box_count(base)` reads a
projectile box unchanged):

| Field | Offset | Meaning |
|---|---|---|
| validity / existence | `+0x2A0` (dword) | vuln-box pointer; `==0` ⇒ invalid/unused slot, non-zero ⇒ live object |
| emitter id | `+0x02` (byte, +1) | which player fired it (byte 1 ⇒ player id 2 = P2) |
| **attack-box active** | `+0x2C8` (dword ptr → 4×8) | the projectile's OWN attack box; same 4-slot layout as a player — the active-hitbox signal |
| X position | `+0x64` (word signed) | projectile world-x, advances toward target after spawn |
| Y position | `+0x68` (word signed) | projectile world-y |
| type | `+0x91` (byte) | projectile type byte (strength-tagged) |
| remaining hits | `+0x9E` (byte) | hit budget (1 for hadouken, >1 for multi-hit supers) |
| freeze | `+0x45` (byte) | projectile freeze/hitstop counter |

Char-force override reused from Session 12 (P2 select-id `0x02011388` while
state<5): Twelve=19, Remy=20, Ibuki=7, Urien=13; in-match SA-slot readback
`0x0201138C` (0=SA1/1=SA2/2=SA3). CPS3 char enum: `src/constants.h:12-34`.

**Phase-2 findings (all hardware-backed):** the projectile-Startup **−1** has
TWO distinct proximate causes, NOT one convention split (corrected 2026-07-17,
lever V):
- **twelve-sa1 S** is the genuine oracle-table convention split
  (strictly-before vs spawn-slot) — `proj_spawn_raw=3` (post-append consume),
  `ryu-sa1` engine S=3 PASS ≡ `twelve-sa1` engine S=3 XFAIL, byte-identical, no
  engine-visible discriminant. STAYS XFAIL (oracle 4, engine 3).
- **remy-sa1 S** (and urien-sa2 S) is an ENGINE slot-0-latch artifact, not a
  table split: `proj_spawn_raw=0` means the projectile spawns ON the MOVE_START
  frame, so the pre-append slot-0 latch set S one convention-frame below the
  post-append consume every other proj super uses. **RESOLVED by lever V**
  (display-only slot-0→post-append harmonization; R keeps the raw proj_s
  anchor): remy-sa1 ×4 flip XFAIL→PASS at oracle Startup=1 — remy is
  tape-anchored (the cap3 tape's strictly-before spawn = 1 == oracle Startup).
  urien-sa2 S now displays oracle 1 with R=91 intact, but this is
  oracle-AGREEMENT only, NOT tape-anchored: urien's CAP-3 tapes show the spawn
  pool-valid on the 3rd post-flash frame (strictly-before = 2), so displayed
  S=1 matches the oracle but not urien's own tape-derived count. **The earlier
  "oracle S+R=92 > T=91, internally over-budget / self-contradictory" claim is
  REFUTED** (digest finding #1, `<sp>/zero-b/b5-window-rederive.txt`): the 91 is
  the ENGINE meter window, not an oracle quantity; the arcade post-flash busy
  window is **94 frames** (flash 256432–256482, busy768 at 256577), and
  strictly-between(spawn 256485, busy768) = 91 = oracle R exactly, leaving 2
  pre-spawn frames so oracle S=1 + R=91 are jointly satisfiable on the arcade
  94-frame timeline. The residual is CROSS-LAYER (the engine's single-slot
  `R = meter_len − proj_s` model cannot place S and R independently) plus an
  **OPEN** engine-measurement item: the engine meter window (91) is 3 frames
  short of the arcade busy window (94) — a possible future RE-ANCHOR/lever-N
  meter re-anchor path, NOT resolved. The row stays XFAIL on its A divergence,
  so no wrong value ships either way.

Other Phase-2 findings (unchanged, stay XFAIL asserting oracle): ibuki-sa3 R
**+2** vs sean-sa1 R **−2** (opposite signs, no uniform proj-R offset);
urien-sa2 beam-A: **arcade attacker T is FLAT 144 over 6× spacing** (refuting
the engine's 11→83 distance-scaling), the projectile box-active window is
travel-dependent 34/85/105 (close/mid/far). These are documented
divergences, not blocked capability — see §13.16 and `proj-split/fit.md`. Rig
files (helpers3.lua, pjderive.py, raw tapes, screenshots) preserved in
`<sp>/zero/cap3/` (scratchpad-only, not committed).

## Session 14 / CAP-4 (host harness, no arcade rig) — Urien Chariot rush whiff-reachability probe (terminal)

**Status: 2026-07-14 (`<sp>/zero/cap4/chariot-impl-report.md`).** Not an arcade
capture — a host-harness forced-whiff probe recording the
SPECIALS-REACHABILITY GAP terminus for `urien-chariot-hk-block/-hit`. A
centered-mode whiff attempt at the harness's absolute distance ceiling
(`dist: 310`, achieving dist=322 at dash-fire — more separation than any
prior probe, anchored or centered) STILL CONNECTS: MOVE_START
`atk_x=333 def_x=655 dist=322`, FINAL `outcome=HIT S=13 A=15 R=20`, not a
WHIFF. Chariot rush (RH)'s dash reach closes the gap and connects across the
entire achievable placement envelope — the reachability gap is now empirically
confirmed, not inferred. No clean whiff R baseline is obtainable via Path A;
the only remaining route (a no-collision dummy-harness driver mode,
feasibility.md §2.5) is a real driver change, out of scope. `urien-chariot`
stays XFAIL, terminal REACHABILITY-GAP. Probe entry was added, measured, then
removed (the corpus never carries a row that can only fail); zero golden
change, suite unchanged at 1,290/59.

## Caveats

- **ROM revision.** This capture ran on `sfiii3` only — Street Fighter
  III 3rd Strike: Fight for the Future, Euro 990608. Other arcade PCB
  revisions of the same game exist in the same MAME/FBNeo ROM set
  (`sfiii3r1`, 990512; `sfiii3nr1`) and were not captured. Every result
  in this document should be read as "confirmed on Euro 990608" — a
  revision-level behavioral difference between 990608 and an earlier or
  regional revision has not been ruled out and is not addressed by
  either session.

  **DATED UPDATE (2026-07-11, LAYER-1).** The revision-level question
  this caveat left open is now measured, not merely "not ruled out":
  Session 3 (above) captured `sfiii3nr1` (Japan 990512 NO CD) and
  re-validated all seven Euro-990608 disputed rows plus 13 controls --
  **990512==990608 on every row tested.** Per the FBNeo driver's own
  ROM tables (`layer/ancestry-findings.md` §1.4, ROM-set CRC
  comparison), whatever gameplay-relevant difference exists between the
  two dated revisions lives entirely inside one program chip, `simm1`;
  `simm2` (the other program chip) and all 40 graphics SIMMs are
  CRC-identical between 990512 and 990608. Region (Japan/US/Europe/
  Asia) and CD/NO-CD are both ruled out as gameplay-data axes by the
  same ROM-set evidence (`layer/ancestry-findings.md` §2.3) -- every
  region/CD variant of a given dated revision shares byte-identical
  SIMM content; only the BIOS/loader chip differs. This does not
  resolve which arcade date the original 2004 PS2 port's own combat
  data descends from (see `docs/frame-data-synthesis.md`'s new §13.16)
  -- that ancestry question stays UNKNOWN -- but it removes revision as
  a live explanation for any row this program's own frame-data suite
  disputes, since both dated revisions now agree with each other
  everywhere measured.

## Follow-up targets for a future capture session

- **Oro Oniyama Jab, whiff R.** `oro-oniyama-lp-whiff`'s masked R value
  (`docs/arcade-frame-data/ERRATA.md` §2) rests on a 2-of-3
  secondary-source majority (`oro.json` + FAT-3S agree Recovery is
  negative; EventHubs alone shows it positive) — a live sign conflict
  between the external sources, not yet settled by a primary capture.
  This rig can settle it in one capture (whiff, dp+LP, neutral
  distance), the same way it settled Sean Dragon Smash Strong.

**DATED UPDATE (2026-07-11, LAYER-1): DONE.** Session 3 (above) settled
this target -- arcade 990512 busy-R = 29 (2 reps, `oro-oniyama-lp-whiff`
recipe), matching the oracle's magnitude (29, oracle notation -29) and
EventHubs' positive 29; see `docs/arcade-frame-data/ERRATA.md` §2's own
dated update. The open follow-up target now is the OVERLAY RE-ANCHOR
program framed in `docs/plan-frame-data-completion.md`'s new LAYER-1
tracker row: a census-first engine-observer program that would
re-derive the overlay's S/A/R from the raw-signal constructions this
run's twin validated, using the twin as a full-suite ground-truth
census instrument. Prerequisites named there (contact-leg hitstop rule
alignment, a PASS-row regression census, selective arcade verification
of any moved PASS row) -- new scope, user-gated, not started.

## Reproduction

1. Fightcade2 must be installed at
   `/Applications/FightCade2.app` with the `sfiii3` ROM present under
   its bundled FBNeo `ROMs/` directory.
2. Build a shadow work directory as described under Rig above (symlinks
   only — never edit files inside the real Fightcade install).
3. Place `bridge.lua` (the control-channel script) in the work
   directory and launch with the exact command under Rig above.
4. From another terminal, drive the bridge by writing sequenced
   commands to `bridge-cmd.txt` and reading acks from `bridge-ack.txt`
   — coin/start, character select (see Navigation notes below), an
   anchor `savestate.create()`, then repeated `savestate.load(anchor)`
   + a queued input + N frames of per-frame logging per capture.
5. Always capture 2 reps per row and `diff` the raw logs — a
   byte-identical diff is the acceptance bar; any difference means the
   savestate anchor or input timing was not actually deterministic and
   the capture must be re-run.

### Navigation notes (session 2, no wedge)

Character-select navigation was done with plain directional taps plus a
window-screenshot name-crop read (Vision OCR could not read the game's
striped select-screen font; the cropped images were read directly
instead). The select-screen timer was frozen (write `0x30` to
`0x020154FB` every frame) during navigation only, and disabled before
each match starts. Select lattice paths used this session: Yang = Ryu
D-x5,L; Urien = Alex-column Up-x4 from Alex, or Necro-then-Up; Q =
Chun-Li-then-Up; Ibuki = Necro-then-Down. P2's Start needs a second
press after P1's select screen has already loaded.

## Session deviations from the ideal recipe

1. **Host-input injection — attempted, then permanently banned.**
   Session 1 initially explored driving character select via macOS-side
   GUI/keystroke automation (synthetic clicks/keystrokes sent to the
   emulator window). This was abandoned mid-session: the user was
   actively using the machine at the time, and synthetic input leaked
   into other frontmost applications (Discord took window focus
   mid-test; at least two synthetic "5" keystrokes may have landed in
   whatever app was frontmost at that moment, not the emulator). Because
   the Lua bridge's own CLI-loader path made host-input automation
   unnecessary anyway (character select can be driven the same way as
   any other input, via `joypad.set` through the bridge), host-side
   input injection was dropped entirely and is not used anywhere in the
   session-2 sweep. This is a hard rule for any future capture session:
   **all inputs go through the Lua bridge; the host input layer
   (keyboard/mouse) is never synthesized.**
2. **Yang Senkyuutai LK motion mis-identification, caught and
   corrected.** The first attempt used dp+LK, which dispatches Zenten
   (a zero-attack-box forward roll, act=00050066) instead of Senkyuutai.
   Detected immediately from the zero-box signature and the corpus's own
   documented recipe (qcf+LK); both reps were re-captured with the
   correct motion. The mis-keyed logs were not archived (superseded in
   place, not silently discarded — recorded here for the record).
3. **Yang Forward whiff-unmeasurable; captured on HIT instead.** Yang
   Forward (close MK) is a close-range move with no true whiff variant
   distinct from its own far normal, so it was captured on HIT at
   point-blank range with a same-anchor freeze-latency control (close
   LP, whose arcade S=3 is agreed by every side) to establish that
   hitstop is sampled on the contact frame itself. The A-window on the
   HIT capture excludes `frz > 0` frames, extending the whiff-only rule
   above to a contact leg — this extension is validated only by its own
   internal consistency (the non-frozen box-frame count independently
   reproduces the oracle/engine-agreed A=5), not by a separate
   whiff-vs-hit control the way the other 8 instrument-validation
   controls are.

## Session index (raw artifacts, scratchpad-only, not in this repo)

The raw per-frame logs, bridge script, OCR/window-list helper scripts,
and screenshots that this document condenses live in the orchestrating
session's scratchpad (`capture/` — `feasibility.md`, `capture-log.md`,
`sweep-results.md`, `cap-*.log` per-move raw dumps) and are not checked
into this repository. This document is the durable, checked-in record;
re-running Reproduction above regenerates the same raw logs if deeper
verification is ever needed.
