#!/usr/bin/env python3
"""Acceptance test for triage_drift_history.py.

Every case here is anchored to real history in this repository, and every one
was hand-verified against raw git before being written down:

    $ git show a752e2ca:src/sf33rd/Source/Game/engine/plcnt.c | sed -n '100p'
    s8 vib_sel[2];
    $ sed -n '100p' src/sf33rd/Source/Game/engine/plcnt.c
    u16 vital_dec_timer;
    $ grep -n vib_sel src/sf33rd/Source/Game/engine/plcnt.c
    102:s8 vib_sel[2];

So `docs/research-desync-deep-investigation.md:186`, which cites
`plcnt.c:100` for `vib_sel`, was CORRECT when written and the code moved: the
textbook DRIFTED case. The test doctors that one real citation to walk every
branch of the classifier, which is the point -- a classifier that returns the
same verdict for a correct citation and a fabricated one is decoration.

The threshold test is the one that matters most. UNPROVABLE-PRE-IMPORT exists
because `a752e2ca` is a 134-file omnibus squash that flattened doc history, so
"not on the cited line there" is not evidence of anything. If that guard ever
stops firing, 453 unprovable findings silently become accusations.
"""

import importlib.util
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

spec = importlib.util.spec_from_file_location("triage", os.path.join(HERE, "triage_drift_history.py"))
triage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(triage)

PLCNT = "src/sf33rd/Source/Game/engine/plcnt.c"
DOC = "docs/research-desync-deep-investigation.md"
IMPORT_SHA = "a752e2ca"

failures = []


def check(label, expect, finding):
    got, detail = triage.classify(finding)
    ok = got == expect
    print(f"  [{'ok' if ok else 'FAIL'}] {label}: expected {expect}, got {got}")
    if not ok:
        failures.append(f"{label}: expected {expect}, got {got} ({detail})")


def cite(path, line, token):
    return {
        "path": DOC,
        "line": 186,
        "message": f"cited {path}:{line} for `{token}`, but that line does not mention it",
    }


def main():
    # Guard the ground truth first. If the repo state these cases are anchored
    # to ever changes, this test must say so loudly rather than quietly assert
    # the wrong thing.
    print("ground truth:")
    then = subprocess.run(
        ["git", "-C", REPO, "show", f"{IMPORT_SHA}:{PLCNT}"],
        stdout=subprocess.PIPE, text=True,
    ).stdout.split("\n")
    if "vib_sel" not in then[99]:
        print(f"  [SKIP] {PLCNT}:100 at {IMPORT_SHA} is no longer `vib_sel`; anchor moved.")
        return 0
    print(f"  [ok] {PLCNT}:100 at {IMPORT_SHA} is: {then[99].strip()}")

    print("classifier branches:")
    check("real citation, code moved", "DRIFTED", cite(PLCNT, 100, "vib_sel"))
    check("off by one", "UNPROVABLE-PRE-IMPORT", cite(PLCNT, 101, "vib_sel"))
    check("points where the symbol lives today", "UNPROVABLE-PRE-IMPORT", cite(PLCNT, 102, "vib_sel"))
    check("line past EOF", "INCONCLUSIVE", cite(PLCNT, 999999, "vib_sel"))
    check("symbol absent from the whole file", "INCONCLUSIVE", cite(PLCNT, 100, "zzz_no_such_symbol"))
    check("cited file never existed", "FILE-ABSENT", cite("src/no_such_file.c", 100, "vib_sel"))
    check("unparseable message", "INCONCLUSIVE", {"path": DOC, "line": 186, "message": "no citation here"})

    # The import guard must be load-bearing. With the threshold raised so that
    # nothing counts as an import, the same citation must be ACCUSED instead of
    # excused -- proving the guard is what is suppressing the accusation.
    print("import guard is load-bearing:")
    saved = triage.IMPORT_FILE_THRESHOLD
    try:
        triage.IMPORT_FILE_THRESHOLD = 10 ** 9
        triage._import_cache.clear()
        got, _ = triage.classify(cite(PLCNT, 101, "vib_sel"))
        ok = got == "NEVER-CORRECT"
        print(f"  [{'ok' if ok else 'FAIL'}] threshold disabled -> {got} (want NEVER-CORRECT)")
        if not ok:
            failures.append(f"import guard not load-bearing: got {got} with the guard disabled")
    finally:
        triage.IMPORT_FILE_THRESHOLD = saved
        triage._import_cache.clear()

    if failures:
        print("\nFAILED:")
        for f in failures:
            print("  -", f)
        return 1
    print("\nall triage acceptance cases pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
