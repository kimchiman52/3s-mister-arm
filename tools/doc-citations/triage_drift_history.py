#!/usr/bin/env python3
"""Decide, for each `drift` finding, whether the citation was EVER correct.

`check_doc_citations.py` reports that a citation no longer resolves. It cannot
say *why*, and the why determines the fix:

  - a citation that was correct when it was written and is wrong now is CODE
    DRIFT. The document is a faithful record; the line number moved underneath
    it. Repointing it is safe and mechanical.

  - a citation that was already wrong on the day it was written was never
    evidence of anything. Repointing it to wherever the symbol happens to live
    today manufactures a provenance that never existed. These need a human.

This tool separates the two by replaying history: it blames the prose line to
find the commit that wrote it, then reads the cited file *at that commit* and
asks whether the token was on the cited line back then.

Verdicts:
  DRIFTED        token was on the cited line at the authoring commit -> code moved.
  NEVER-CORRECT  the cited file existed at the authoring commit and the token
                 was demonstrably elsewhere in it -> the citation was wrong when
                 written.
  CORRECT-AT-PARENT
                 the citation resolves at the blamed commit's PARENT. The
                 document tabulates the state its own commit replaced, which is
                 what plan documents do. NOT a defect -- repointing it would
                 destroy the record.
  FILE-ABSENT    the cited path did not exist at the authoring commit.
  UNCOMMITTED    the prose line has uncommitted working-tree edits, so there is
                 no authoring commit to consult yet.
  UNPROVABLE-PRE-IMPORT
                 the token was elsewhere in the file at the blamed commit, BUT
                 that commit is a bulk history-flattening import rather than the
                 line's real authoring event. See below.
  INCONCLUSIVE   history could not answer (file unreadable at that commit, line
                 out of range then, token absent from the whole file then).
                 Absence of proof is not proof: these are NOT counted as defects.

INCONCLUSIVE is a first-class verdict on purpose. The alternative -- guessing --
is how the citations being triaged got wrong in the first place.

WHY UNPROVABLE-PRE-IMPORT EXISTS
This branch was assembled by omnibus squashes. `a752e2ca` ("new stuff and
performance improvements", 134 files, +30142 lines) is the commit that ADDED
most of the research and plan documents, and it flattened whatever per-document
history preceded it. For a line blamed on such a commit:

  - "the token WAS on the cited line there" still proves drift. That is the
    earliest state this repository can observe, and the citation resolved in it.
  - "the token was NOT on the cited line there" proves nothing. The document may
    have been written weeks earlier against code that then moved before the
    squash. That history does not exist in this repo and cannot be consulted.

Calling the second case NEVER-CORRECT would be exactly the sin this tool exists
to catch: asserting a provenance the evidence does not support. So it gets its
own verdict, and it is not counted as a defect.

An import is identified structurally -- by how many files the commit touches --
not by a hardcoded sha, so the classification keeps working as history grows.

Usage:
  python3 tools/doc-citations/triage_drift_history.py            # lint, then triage
  python3 tools/doc-citations/triage_drift_history.py --json IN  # triage a saved --json run
  python3 tools/doc-citations/triage_drift_history.py --tsv OUT  # per-finding verdicts
"""

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# "cited src/netplay/direct_p2p.c:150 for `remote_port`, but that line does not mention it"
MSG_RE = re.compile(r"cited\s+(\S+?):(\d+)\s+for\s+`([^`]+)`")


def git(*args, binary=False):
    """Run git in the repo; return stdout, or None when git refuses."""
    p = subprocess.run(
        ["git", "-C", REPO] + list(args),
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    if p.returncode != 0:
        return None
    return p.stdout if binary else p.stdout.decode("utf-8", "replace")


UNCOMMITTED = "<uncommitted>"

_blame_cache = {}


def blame_commit(path, line):
    """Commit that last touched `line` of `path`. None if git cannot say."""
    key = (path, line)
    if key in _blame_cache:
        return _blame_cache[key]
    out = git("blame", "-L", f"{line},{line}", "--porcelain", "--", path)
    sha = out.split()[0] if out else None
    if sha and not re.fullmatch(r"[0-9a-f]{40}", sha):
        sha = None
    # git blame reports an all-zero sha for a line that is modified in the
    # working tree but not committed. That is a valid 40-char hex string, so it
    # sails through the check above and then makes every `git show` fail --
    # which silently reads as "the cited file did not exist", turning an
    # unsaved edit into a fabricated verdict. Catch it explicitly.
    if sha and set(sha) == {"0"}:
        sha = UNCOMMITTED
    _blame_cache[key] = sha
    return sha


# A commit touching more files than this is treated as a bulk import that
# flattened history, not as the authoring event for any one line in it.
# a752e2ca (134 files) is the omnibus squash that introduced most of docs/;
# ordinary doc-authoring commits on this tree touch single digits of files.
IMPORT_FILE_THRESHOLD = 30

_import_cache = {}


def is_bulk_import(sha):
    if sha in _import_cache:
        return _import_cache[sha]
    out = git("show", "--numstat", "--format=", sha)
    n = len([l for l in out.split("\n") if l.strip()]) if out else 0
    v = n > IMPORT_FILE_THRESHOLD
    _import_cache[sha] = v
    return v


_show_cache = {}


def file_at(sha, path):
    """Lines of `path` as of `sha`; None if the path did not exist there."""
    key = (sha, path)
    if key in _show_cache:
        return _show_cache[key]
    out = git("show", f"{sha}:{path}")
    lines = out.split("\n") if out is not None else None
    if len(_show_cache) > 4000:  # these are whole source files; do not grow forever
        _show_cache.clear()
    _show_cache[key] = lines
    return lines


def token_on(lines, lineno, token):
    if lines is None or lineno < 1 or lineno > len(lines):
        return None
    return token in lines[lineno - 1]


def classify(finding):
    """-> (verdict, detail)."""
    m = MSG_RE.search(finding.get("message", ""))
    if not m:
        return "INCONCLUSIVE", "message did not parse"
    cited_path, cited_line, token = m.group(1), int(m.group(2)), m.group(3)

    sha = blame_commit(finding["path"], finding["line"])
    if sha is None:
        return "INCONCLUSIVE", "no blame for the prose line"
    if sha is UNCOMMITTED:
        return "UNCOMMITTED", "the prose line has uncommitted edits; commit them and re-run"

    then = file_at(sha, cited_path)
    if then is None:
        return "FILE-ABSENT", f"{cited_path} absent at {sha[:8]}"

    hit = token_on(then, cited_line, token)
    if hit is True:
        return "DRIFTED", f"`{token}` was on {cited_path}:{cited_line} at {sha[:8]}"
    if hit is None:
        return "INCONCLUSIVE", f"{cited_path} had {len(then)} lines at {sha[:8]}"

    # The token was not on the cited line then. Only an accusation if we can
    # exhibit it somewhere else in that same file at that same commit -- the
    # linter's own "drift is proven, never inferred" rule, applied to history.
    # A plan document routinely tabulates the state its own change is about to
    # replace. Such a document ships in the SAME commit as the change, so the
    # blamed commit is the state AFTER the thing being described was removed.
    # Check the parent before accusing it of anything: if the citation resolves
    # there, the document is an accurate record of the "before" and repointing
    # it to today's code would destroy the very thing it documents.
    before = file_at(sha + "^", cited_path)
    if token_on(before, cited_line, token) is True:
        return (
            "CORRECT-AT-PARENT",
            f"`{token}` was on {cited_path}:{cited_line} at {sha[:8]}^ -- the doc records "
            f"the state its own commit replaced; do NOT repoint",
        )

    elsewhere = [i + 1 for i, t in enumerate(then) if token in t]
    if not elsewhere:
        return "INCONCLUSIVE", f"`{token}` absent from all of {cited_path} at {sha[:8]}"
    near = ", ".join(str(x) for x in elsewhere[:3])
    detail = f"`{token}` was at {cited_path}:{near} at {sha[:8]}, not :{cited_line}"

    # The blamed commit is only evidence of the AUTHORING state if it is the
    # authoring event. A bulk import is not: it flattened the history in which
    # this citation may well have been correct.
    if is_bulk_import(sha):
        return "UNPROVABLE-PRE-IMPORT", detail + f"; {sha[:8]} is a bulk import, pre-import history is gone"
    return "NEVER-CORRECT", detail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", help="read a saved check_doc_citations.py --json run")
    ap.add_argument("--tsv", help="write per-finding verdicts here")
    ap.add_argument("--code", default="drift", help="finding code to triage")
    args = ap.parse_args()

    if args.json:
        data = json.load(open(args.json))
    else:
        p = subprocess.run(
            [sys.executable, os.path.join(HERE, "check_doc_citations.py"), "--json"],
            stdout=subprocess.PIPE, cwd=REPO,
        )
        data = json.loads(p.stdout.decode("utf-8", "replace"))

    findings = [f for f in data["findings"] if f["code"] == args.code]
    verdicts = Counter()
    per_file = defaultdict(Counter)
    rows = []
    for f in findings:
        v, detail = classify(f)
        verdicts[v] += 1
        per_file[f["path"]][v] += 1
        rows.append((v, f["path"], f["line"], detail, f.get("suggestion") or ""))

    if args.tsv:
        with open(args.tsv, "w") as fh:
            fh.write("verdict\tdoc\tdoc_line\tdetail\tsuggestion\n")
            for r in rows:
                fh.write("\t".join(str(x).replace("\t", " ") for x in r) + "\n")

    for v, n in verdicts.most_common():
        print(f"{n:6d}  {v}")
    print()
    hdr = ["DRIFTED", "NEVER-CORRECT", "CORRECT-AT-PARENT", "UNPROVABLE-PRE-IMPORT", "FILE-ABSENT", "INCONCLUSIVE", "UNCOMMITTED"]
    print("doc".ljust(56) + "".join(h[:6].rjust(9) for h in hdr))
    for path, c in sorted(per_file.items(), key=lambda kv: -sum(kv[1].values())):
        print(path.ljust(56) + "".join(str(c[h]).rjust(9) for h in hdr))
    print(
        "\nDRIFTTRIAGE SUMMARY: code=%s total=%d %s"
        % (args.code, len(findings), " ".join(f"{k}={v}" for k, v in sorted(verdicts.items())))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
