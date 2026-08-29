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
IMPORT_SHA_FULL = "a752e2caf3151f197e2281aca66f1852cee047b7"

failures = []


def check(label, expect, finding, sha=None):
    # Pin the blame result. The classifier's job is to reason from an authoring
    # commit to a verdict, and that is what is under test; which commit git
    # happens to blame a live doc line to is not. Leaving it live made the test
    # fail merely because the anchor document had unstaged edits, which is a
    # property of the working tree rather than of the logic.
    triage._blame_cache[(finding["path"], finding["line"])] = sha or IMPORT_SHA_FULL
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

    # A plan document tabulates the state its own commit replaces. 71694d40
    # ("split FPS overlay into FPS mode and Debug mode") both landed the change
    # and carried the plan whose table cites the PRE-change lines:
    #   $ git show 71694d40^:src/port/sdl/sdl_app.c | sed -n '86p'
    #   static bool show_fps_overlay = false;
    # Repointing that to today's code would destroy the record, so it must not
    # be reported as a defect.
    print("plan documents that record the state they replaced:")
    split = {"path": "docs/archive/plan-fps-overlay-split.md", "line": 60,
             "message": "cited src/port/sdl/sdl_app.c:89 for `fps_overlay_value`, "
                        "but that line does not mention it"}
    split_sha = subprocess.run(["git", "-C", REPO, "rev-parse", "71694d40"],
                               stdout=subprocess.PIPE, text=True).stdout.strip()
    check("plan table citing the pre-change state", "CORRECT-AT-PARENT", split, sha=split_sha)

    # "The token moved" is not "the statement survived". At e37d6208,
    # plmain.c:726 was `wk->sa->store -= -1;`; upstream decomp re-check
    # ad411df5 turned it into `wk->sa->store -= 1;`. Zero occurrences of the
    # original remain, so a repoint would leave a resolving citation inside a
    # paragraph reasoning about a sign that no longer exists.
    print("statements that changed rather than moved:")
    flip_sha = subprocess.run(["git", "-C", REPO, "rev-parse", "e37d6208"],
                              stdout=subprocess.PIPE, text=True).stdout.strip()
    if flip_sha:
        flip = {"path": "docs/plan-frame-data-completion.md", "line": 977,
                "message": "cited src/sf33rd/Source/Game/engine/plmain.c:726 for "
                           "`store`, but that line does not mention it"}
        check("sign flip under a still-resolving symbol",
              "DRIFTED-STATEMENT-CHANGED", flip, sha=flip_sha)
    else:
        print("  [SKIP] e37d6208 not present in this clone")

    # The import guard must be load-bearing. With the threshold raised so that
    # nothing counts as an import, the same citation must be ACCUSED instead of
    # excused -- proving the guard is what is suppressing the accusation.
    print("import guard is load-bearing:")
    saved = triage.IMPORT_FILE_THRESHOLD
    try:
        triage.IMPORT_FILE_THRESHOLD = 10 ** 9
        triage._import_cache.clear()
        f = cite(PLCNT, 101, "vib_sel")
        triage._blame_cache[(f["path"], f["line"])] = IMPORT_SHA_FULL
        got, _ = triage.classify(f)
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
