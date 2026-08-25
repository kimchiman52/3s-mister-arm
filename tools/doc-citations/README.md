# Documentation citation linter

Checks that the citations written in this repo's prose still point at something
that exists: `file.c:123` line citations, backticked symbol names, and
referenced paths, in `docs/**.md`, the top-level `*.md`, and the comment text
inside `src/`, `include/` and `tools/`.

It checks whether a claim's evidence is **reachable**, not whether the claim is
**true**. Only the first is mechanical.

```sh
python3 tools/doc-citations/check_doc_citations.py              # whole tree
python3 tools/doc-citations/check_doc_citations.py docs/        # one subtree
python3 tools/doc-citations/check_doc_citations.py --errors-only
python3 tools/doc-citations/check_doc_citations.py --json
python3 tools/doc-citations/test_doc_citations.py               # acceptance test
```

Exit codes: `0` no errors, `1` errors, `2` harness failure. The last line is
always a machine-greppable `DOCCITE SUMMARY:` verdict.

**This file states no finding counts.** A README that says "there are 412
findings" is a documentation defect waiting to happen — exactly the thing this
tool exists to catch. Run the tool; the number comes from the summary line.

## Finding codes

Severity is set by **measured precision**, not by how bad the failure sounds.
Every figure below comes from hand-auditing sampled findings against the actual
files; the sample size is given so you can weigh it.

| code | severity | precision | meaning |
| --- | --- | --- | --- |
| `degenerate-target` | error | **9/9** | the cited line is blank, or only a brace. Nobody cites those on purpose. |
| `drift` | error | 23/40 | file and line exist, but the line no longer contains the symbol the citation is about. Carries the line where the symbol actually is. |
| `wrong-path` | error† | 5/9 | the path has never existed, but a file with that basename does — carries the suggestion. |
| `line-out-of-range` | error | 4/9 | the file has fewer lines than the citation claims. |
| `phantom-path` | advisory | **0/9** | no commit on any ref has ever contained this path. |
| `phantom-identifier` | advisory | **1/9** | a backticked symbol absent from all current code *and* all historical revisions. |
| `stale-path` | advisory | — | the path existed and was deleted. A historical record, not a fabrication. |
| `stale-identifier` | advisory | — | ditto for a symbol. |
| `unresolvable-evidence` | advisory | — | "see the *X* report" where no path appears anywhere in the sentence. |

† demoted to advisory in RECORD-class documents — see `record-documents.txt`.

**`phantom-path` and `phantom-identifier` are advisory because they measured
0/9 and 1/9.** They stay switched on — `phantom-identifier` is the check that
catches the `kill_texcash_work` class, and that class is worth finding — but a
category that is wrong four times out of five must not be able to fail a build.
Their residue is not a scoping bug awaiting one more rule: it is documents whose
citations are relative to a different worktree, informal scratchpad artifacts,
external tool and NEON intrinsic names, and numbered-family placeholders like
`nm_NNNNN`. Those are things this tree genuinely cannot resolve, and none of
them is a defect. Promote them only together with a fresh measurement.

`line-out-of-range` measuring 4/9 was the surprise — a citation into a file that
is demonstrably too short still looks certain, but most of the misses are docs
that cite a *sibling worktree*. That is why the table exists.

## The three ideas that make it quiet enough to use

**1. Never-existed is a defect; existed-and-was-deleted is not.** A plan
document citing a file the renderer rip-out deleted is a historical record.
A brief citing `tools/mister/build-deps.sh`, a path no commit has ever
contained, is a fabrication. The linter tells them apart by reading git
history, and only the second is an error. This is what took the finding count
from unusable to triageable.

**2. Drift is proven, never inferred.** There are no maintained fingerprints —
a hash a human has to update would rot exactly the way the comments rotted.
Instead the linter takes identifier tokens the author already wrote next to the
citation and asks whether any appears on the cited line. When it cannot find
the token anywhere in the file it stays **silent**: absence of proof is not a
finding. It reports drift only when it can exhibit the token elsewhere in the
same file, so every drift finding carries its own evidence and a one-line fix.

**3. Exoneration is generous, accusation is strict.** A citation is cleared if
*any* symbol from the surrounding prose lands on the cited line; it is accused
only using the tokens nearest to it. Sentences carry several citations, and
letting one borrow its neighbour's subject was the largest false-positive
source measured.

## Configuration

Three files, each requiring a written reason per entry:

- `allowlist.txt` — identifiers that are legitimately not symbols of this repo.
- `record-documents.txt` — globs for proposals and superseded write-ups, where
  naming a not-yet-existing thing is the document's job.
- `external-paths.txt` — paths that deliberately point at another repository or
  another machine, which no amount of checking this tree can resolve.

Keep all three short. Every entry is a place the linter has been told to stop
looking.

## Known limits

- **Drift precision is 23/40** on this tree, from two independent hand audits
  (110 findings were audited in total across five audits; the earlier 70 scored
  38/70 against a version before the anchor-ownership fix). That is a triage
  tool, not a gate. Do not run `--fix` over drift findings unattended; read the
  evidence line, which is printed precisely so you can judge in one glance.
- **`degenerate-target` is the only check that measured perfect** (9/9). If you
  want one thing wired into CI, it is that one.
- **A citation with no symbol near it cannot be drift-checked.** `texcash.c:301/516/554`
  in a sentence whose subject is `be` gets its line numbers range-checked and
  nothing more. This is asserted as a known gap by the acceptance test so that
  it cannot be quietly forgotten.
- **A symbol renamed inside a file that still exists** is reported as a phantom
  rather than as stale. Deliberate: a reader greping for it finds nothing
  either way.
- **Cross-repository citations are indistinguishable** from local ones and must
  be listed in `external-paths.txt` by hand.
- `vendor/` and `src/imgui/` are not scanned — third-party line numbering we do
  not control — but both remain resolvable *targets*.

## Testing

`test_doc_citations.py` runs the linter against two fixtures: the six citation
defects found by hand on 2026-08-24 (all must be caught) and a set of correct
citations of every shape (none may be reported). Both halves matter — a checker
that reports everything catches all six too.

The test was confirmed able to fail by mutation: neutering the history corpus,
inverting the drift comparison, and making the path resolver accept anything
each turn it red.

## Baselining

The tree carries a large pre-existing backlog, so a gate must fail only on
*new* findings:

```sh
python3 tools/doc-citations/check_doc_citations.py --write-baseline b.json
python3 tools/doc-citations/check_doc_citations.py --baseline b.json   # exit 1 only on new errors
```

The baseline is a derived artifact and is **not** committed (`.gitignore`) —
a checked-in list of known-bad citations would itself go stale, which is the
failure this tool exists to catch. Generate it in CI at the pinned commit.
