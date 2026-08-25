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


def run(*args):
    proc = subprocess.run(
        [sys.executable, CHECKER, "--json", "--include-testdata"] + list(args),
        cwd=REPO_ROOT, capture_output=True, text=True)
    if proc.returncode not in (0, 1):
        sys.stderr.write("checker failed (rc=%d):\n%s\n"
                         % (proc.returncode, proc.stderr))
        raise SystemExit(2)
    return json.loads(proc.stdout)["findings"]


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
