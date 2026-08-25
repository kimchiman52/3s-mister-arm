# Fixture: correct citations of every shape the linter understands

NOT A REAL DOCUMENT. Everything here is CORRECT as of the tree this fixture
lives in, and test_doc_citations.py requires the linter to report NOTHING.

This is the half of the acceptance test that stops the tool from becoming the
flaky gate it was written to avoid. Catching all six known-bad citations is
easy if you are willing to report everything; staying silent here is what makes
the catching mean something. Each line below is a shape that an earlier draft
of this linter got wrong.

## Slash path with a line number, anchored by a backticked symbol

`spmv_ng_save` is declared at `src/sf33rd/Source/Game/effect/effl8.c:13`.

## The same citation written without the src/ prefix

`spmv_ng_save` is declared at `sf33rd/Source/Game/effect/effl8.c:13`.

## A range citation whose subject appears inside the range

`purge_texture_group` clears ok before calling `Push_ramcnt_key`, at
`src/sf33rd/Source/Game/rendering/texgroup.c:561-563`.

## A bare filename with a line number, the in-tree comment style

The re-entry is the purge_texture_group call at ramcnt.c:102.

## A citation to a call site rather than to a definition

`Push_ramcnt_key` is called for the group key at texgroup.c:483.

## A path with no line number

Desktop dependencies are installed by `build-deps.sh`, and the runbook is
`docs/mister-runbook.md`.

## A filename that is not a symbol

The renderer notes live in `docs/building.md`; do not read `build-deps.sh` as
an identifier.

## An evidence reference that names its artifact by path

Every assertion was proven able to go red; see `docs/rollback-determinism-harness.md`
for the patch-to-red table.

## Backticked things that are not symbols of this repo

Build with `--fast`, set `RUNTIME_INSTALL_PREFIX`, and read `AGENTS.md`.
