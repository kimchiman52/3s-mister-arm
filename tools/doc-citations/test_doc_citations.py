#!/usr/bin/env python3
"""Acceptance test for check_doc_citations.py.

WHAT THIS ASSERTS
-----------------
That the linter still catches the citation defects that were found BY HAND on
2026-08-24, one per case in testdata/known-bad-citations.md. Those six are the
reason the tool exists; a change that stops catching one of them has removed
the only capability that was ever demonstrated.

The test is written so that it FAILS if the linter goes quiet. It asserts, for
each fixture case, that a finding of a specific code lands on a specific line.
It is not a smoke test: deleting any single check, or breaking the anchor
logic, the history corpus, or the path resolver, turns it red. That was
verified by mutation rather than assumed -- see the run recorded in the task
#77 write-up, where inverting the drift comparison and neutering the history
corpus were each confirmed to produce failures.

There is also a NEGATIVE fixture. A checker that reports everything catches all
six too, and would be worthless. testdata/good-citations.md contains correct
citations of every shape the linter understands, and the test requires ZERO
findings on it. Recall and silence are asserted together.

Run:  python3 tools/doc-citations/test_doc_citations.py
Exit: 0 all assertions hold; 1 an assertion failed; 2 harness failure.
"""

import json
import os
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
CHECKER = os.path.join(SCRIPT_DIR, "check_doc_citations.py")
BAD = "tools/doc-citations/testdata/known-bad-citations.md"
GOOD = "tools/doc-citations/testdata/good-citations.md"

# Each entry: (case label, finding code, substring that must appear in the
# finding's message). Line numbers are deliberately NOT asserted -- editing a
# comment in the fixture would then break the test for no reason, which is the
# kind of brittleness that gets tests deleted.
MUST_CATCH = [
    ("1 symbol that never existed",
     "phantom-identifier", "kill_texcash_work"),
    ("2 evidence cited to a document that never existed",
     "unresolvable-evidence", "S7 task report"),
    ("3 line citation that drifted (effl8.c:11 -> :13)",
     "drift", "effl8.c:11"),
    ("5 purge ordering cited at a lone closing brace",
     "drift", "texgroup.c:440-443"),
    ("6 path that never existed at that location",
     "wrong-path", "tools/mister/build-deps.sh"),
]

# Case 4 is a documented RECALL GAP, asserted as such so that it cannot be
# quietly forgotten. Its prose ("... texcash.c:301/516/554 and PPGWork.c:177
# all skip on be == 0") names no symbol the identifier scoping admits -- its
# real subject is `be`, two characters -- and none of its three target lines is
# blank or a brace. The linter range-checks all three numbers and stays silent
# about drift, which is the designed behaviour: no anchor, no accusation.
# If a future change gives this case a finding, that is an improvement, and
# this assertion is what will tell you it happened.
KNOWN_GAP = ("4 multi-number citation with no symbol anchor",
             "texcash.c:301")

# Case 5's SECOND citation, `ramcnt.c:102`, is a documented CORRECT citation,
# not a defect -- it is the exact scenario anchor_tokens' "Asymmetric on
# purpose" docstring describes by name (same shape, real file texgroup.c:287):
# the nearest token to the citation is `Push_ramcnt_key_original_2`, but the
# cited line really is the `purge_texture_group` re-entry, so the citation is
# correct and must stay SILENT.
#
# Before the per-line anchor-window fix (task #84), that exoneration could not
# reach it: `group_num`, the token that proves the cited line is right, sits
# one line above the citation, and the anchor window was exactly one line
# wide. Verified by reverting the window widening alone (anchor_tokens'
# markdown branch, MD_ANCHOR_WINDOW_LINES) and rerunning this fixture: the
# same-line-only window produces a spurious `drift` finding here suggesting
# `ramcnt.c:102 -> ramcnt.c:93`, i.e. it recommends "fixing" a citation that
# was never broken. That is the "mis-paired" defect and the "one citation
# needed a manual reflow" cost described for task #84 -- reflowing the prose
# onto one line was the only way to get the checker to agree it was correct.
NEGATIVE_CASES = [
    ("5b ramcnt.c:102 exonerated by an anchor one line above the citation",
     "ramcnt.c:102"),
]


def run(*args):
    proc = subprocess.run(
        [sys.executable, CHECKER, "--json", "--include-testdata"] + list(args),
        cwd=REPO_ROOT, capture_output=True, text=True)
    if proc.returncode not in (0, 1):
        sys.stderr.write("checker failed (rc=%d):\n%s\n"
                         % (proc.returncode, proc.stderr))
        raise SystemExit(2)
    return json.loads(proc.stdout)["findings"]


def check_fix_refuses_range_citations():
    """Regression for task #84: `--fix` corrupting range citations.

    Observed bug: `--fix` rewrote only the START of a `file.c:LO-HI` drift
    finding (the anchor only ever locates one line) and left the OLD END in
    place, producing a backwards range pointing nowhere -- `522-566` became
    `2360-566` in the wild, across six citations in one file, silently.

    This reproduces the exact mechanism using the SAME drifted citations as
    known-bad-citations.md cases 3 and 5 (`effl8.c:11` and the range
    `texgroup.c:440-443`), copied into a throwaway linked git worktree so
    `--fix`'s in-place edit never has to touch a tracked file. Confirmed by
    hand before the fix landed: running `--fix` on the real fixture rewrote
    `texgroup.c:440-443` to `texgroup.c:483-443` (start moved, end frozen --
    start > end).

    The fixed behaviour: `--fix` refuses the range citation outright (leaves
    the line byte-for-byte unchanged) while still repairing the ordinary
    single-line drift in the same run, proving the refusal is targeted and
    not a case of --fix having stopped working altogether.
    """
    wt_parent = tempfile.mkdtemp(prefix="doccite-fix-test-")
    wt_dir = os.path.join(wt_parent, "wt")
    failures = []
    try:
        subprocess.run(
            ["git", "worktree", "add", "--detach", wt_dir, "HEAD"],
            cwd=REPO_ROOT, capture_output=True, text=True, check=True)

        fixture_rel = "tools/doc-citations/testdata/fix-range-regression.md"
        fixture_path = os.path.join(wt_dir, fixture_rel)
        content = (
            "# Fixture: --fix must refuse range citations (task #84)\n\n"
            "NOT A REAL DOCUMENT. Reuses the exact drifted citations from "
            "known-bad-citations.md cases 3 and 5 in a throwaway worktree, so "
            "this test never writes to a tracked fixture.\n\n"
            "| Symbol | Declared |\n"
            "| --- | --- |\n"
            "| `spmv_ng_save[2]` (u32) | "
            "`sf33rd/Source/Game/effect/effl8.c:11` |\n\n"
            "The nested purge is bounded: purge_texture_group clears ok\n"
            "before calling Push_ramcnt_key (texgroup.c:440-443), so the\n"
            "purge_texture_group(group_num) re-entry inside\n"
            "Push_ramcnt_key_original_2 (ramcnt.c:102) sees ok == 0.\n"
        )
        with open(fixture_path, "w", encoding="utf-8") as fh:
            fh.write(content)
        subprocess.run(["git", "add", fixture_rel], cwd=wt_dir, check=True,
                       capture_output=True)

        proc = subprocess.run(
            [sys.executable, CHECKER, "--root", wt_dir, "--fix",
             "--include-testdata", fixture_rel],
            cwd=wt_dir, capture_output=True, text=True)
        if proc.returncode not in (0, 1):
            sys.stderr.write("checker (--fix) failed (rc=%d):\n%s\n"
                             % (proc.returncode, proc.stderr))
            raise SystemExit(2)

        with open(fixture_path, encoding="utf-8") as fh:
            fixed = fh.read()

        if "texgroup.c:440-443" not in fixed:
            # Generic on purpose: does not assume the corrupted start is any
            # particular number (e.g. the "483" observed by hand), so this
            # still catches the bug even if the real anchor's line moves.
            failures.append(
                "RANGE CITATION WAS ALTERED: --fix must leave "
                "`texgroup.c:440-443` byte-for-byte untouched (refuse, don't "
                "guess); file now reads: %r" % fixed)
        if "effl8.c:13" not in fixed:
            failures.append(
                "--fix did not repair the ordinary single-line drift "
                "(effl8.c:11 -> :13) in the same run -- refusal of the range "
                "citation should not disable fixing elsewhere: %r" % fixed)
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", wt_dir],
                       cwd=REPO_ROOT, capture_output=True)
    return failures


def main():
    failures = []

    findings = run(BAD)
    for label, code, needle in MUST_CATCH:
        hit = [f for f in findings
               if f["code"] == code and needle in f["message"]]
        if not hit:
            failures.append(
                "NOT CAUGHT: case %s -- expected a %s finding mentioning %r; "
                "got %s" % (label, code, needle,
                            sorted({f["code"] for f in findings}) or "nothing"))

    gap_label, gap_needle = KNOWN_GAP
    if any(gap_needle in f["message"] for f in findings):
        failures.append(
            "KNOWN GAP CLOSED: case %s now produces a finding. That is good "
            "news -- promote it into MUST_CATCH and delete this assertion."
            % gap_label)

    for label, needle in NEGATIVE_CASES:
        hit = [f for f in findings if needle in f["message"]]
        if hit:
            failures.append(
                "SPURIOUS FINDING: case %s -- expected no finding mentioning "
                "%r, got %s" % (label, needle,
                                [(f["code"], f["message"]) for f in hit]))

    failures.extend(check_fix_refuses_range_citations())

    good = run(GOOD)
    for f in good:
        failures.append(
            "FALSE POSITIVE on the good fixture: %s:%d [%s] %s"
            % (f["path"], f["line"], f["code"], f["message"]))

    for line in failures:
        print("FAIL: " + line)
    print("DOCCITE TEST: must_catch=%d/%d good_fixture_findings=%d failures=%d"
          % (len(MUST_CATCH) - sum(1 for x in failures
                                   if x.startswith("NOT CAUGHT")),
             len(MUST_CATCH), len(good), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
