#!/usr/bin/env python3
"""Enforce the doc-citation error ceilings recorded in baselines.txt.

Cleaning a scope's citations is easy to do and easy to lose. The two scopes in
`baselines.txt` were each cleaned by hand and were then protected by nothing
but a sentence in a task brief -- and a protection that lives in prose is a
protection that stops being true without telling anybody.

So the ceilings are checked instead of asserted. Exit 1 when a scope is over
its ceiling; exit 1 as well when a scope is comfortably UNDER it, because a
ceiling nobody has tightened is a ceiling that has stopped measuring anything.

    python3 tools/doc-citations/check_baselines.py
    python3 tools/doc-citations/check_baselines.py --quiet

Exit codes: 0 all ceilings hold exactly, 1 a ceiling was breached or has gone
slack, 2 harness failure.
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
LINTER = os.path.join(HERE, "check_doc_citations.py")
BASELINES = os.path.join(HERE, "baselines.txt")


# A scope of this name means the whole repository rather than a path prefix.
# Needed because some classes of defect are not confined to a directory: the
# citations task #110 made visible are spread across fifteen documents, and a
# ceiling that only covered one of them would not protect the class.
TREE_SCOPE = "<tree>"


def load_baselines(path):
    """Parse `<max_errors> <scope> [code]` lines.

    The optional third field narrows a ceiling to ONE finding code. Without it a
    ceiling covers every error in the scope, which is right for a directory that
    has been cleaned outright, and wrong for a defect class that is spread
    tree-wide and being worked down. Both are ratchets; they differ only in what
    they hold still.
    """
    out = []
    for n, raw in enumerate(open(path), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) not in (2, 3) or not parts[0].isdigit():
            raise SystemExit(
                f"{path}:{n}: expected '<max_errors> <scope> [code]', "
                f"got: {raw.strip()}")
        out.append((int(parts[0]), parts[1],
                    parts[2] if len(parts) == 3 else None))
    if not out:
        raise SystemExit(f"{path}: no baselines defined -- refusing to pass vacuously")
    return out


def errors_in(scope, code=None):
    """Error count for a scope, via the linter's own --json output."""
    argv = [sys.executable, LINTER, "--json"]
    if scope != TREE_SCOPE:
        argv.append(scope)
    p = subprocess.run(
        argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=REPO,
    )
    try:
        data = json.loads(p.stdout.decode("utf-8", "replace"))
    except ValueError:
        sys.stderr.write(p.stderr.decode("utf-8", "replace"))
        raise SystemExit(2, f"linter produced no JSON for scope {scope}")
    return sum(1 for f in data["findings"]
               if f["severity"] == "error"
               and (code is None or f.get("code") == code))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(LINTER):
        print(f"BASELINE SUMMARY: harness-failure linter-missing={LINTER}")
        return 2

    bad = []
    slack = []
    results = []
    for ceiling, scope, code in load_baselines(BASELINES):
        label = scope if code is None else f"{scope} [{code}]"
        if scope != TREE_SCOPE:
            target = os.path.join(REPO, scope)
            if not os.path.exists(target):
                bad.append(f"{label}: baseline names a path that does not exist")
                results.append((label, ceiling, None))
                continue
        n = errors_in(scope, code)
        results.append((label, ceiling, n))
        if n > ceiling:
            bad.append(f"{label}: {n} errors, ceiling is {ceiling} (+{n - ceiling})")
        elif n < ceiling:
            slack.append(f"{label}: {n} errors but ceiling is {ceiling} -- tighten it to {n}")

    if not args.quiet:
        for label, ceiling, n in results:
            got = "?" if n is None else str(n)
            mark = "ok " if n == ceiling else "BAD"
            print(f"  [{mark}] {label}: {got} errors, ceiling {ceiling}")

    for m in bad:
        print(f"BREACH: {m}")
    for m in slack:
        print(f"SLACK: {m}")

    print(
        "BASELINE SUMMARY: scopes=%d breached=%d slack=%d"
        % (len(results), len(bad), len(slack))
    )
    return 1 if (bad or slack) else 0


if __name__ == "__main__":
    sys.exit(main())
