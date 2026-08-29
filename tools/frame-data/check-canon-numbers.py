#!/usr/bin/env python3
"""Derive the frame-data verification canon from the golden tables, and
check that the *live* figures written in the docs still agree with it.

THE PROBLEM
-----------
The verification canon -- "94 corpora GREEN, 1349 total rows = 1296 PASS
+ 53 XFAIL" -- is retyped by hand in several documents. The moment a
lever flips a row it is wrong everywhere at once, and nothing notices.
The tree already carries a dozen superseded variants (1,251 / 1,266+81 /
1,270+77 / 1,288+61 / 1,289+60 / 1,290+59 / 1,294+55 / 1,319 / 1,326 /
1,328 / 1,347 rows), so a reader cannot tell a current claim from a
stale one by looking at it.

SOURCE OF TRUTH
---------------
tools/frame-data/golden/*.tsv -- the per-corpus golden tables that
`run-suite.sh --check-golden` diffs every run against. One file per
corpus, one data row per expected.json label, with the verdict
check_frame_data.evaluate_entry computed in column `verdict` (see
golden.py's module docstring). The canon is therefore not a number
anybody types: it is

    corpora = number of golden/*.tsv files
    rows    = sum of data rows across them
    pass    = rows whose verdict == PASS
    xfail   = rows whose verdict == XFAIL

summed here, per the task rule that the canon must be obtained by
SUMMING the per-corpus tables rather than read off one summary line.

`corpus-smoke.yaml` has no golden file and is excluded from the default
suite by run-suite.sh (`--include-smoke` opts it back in), so the
corpora count is the golden set, not the corpus-*.yaml count (95).

WHY AN EXPLICIT PER-NUMBER MARKER
---------------------------------
These docs are append-only engineering logs. The overwhelming majority
of their number mentions sit inside dated entries -- "Suite delta:
1,270/77/1,347 -> 1,288 PASS / 61 XFAIL / 1,349 rows" -- which are
correct HISTORICAL records of a past state. Rewriting those would
destroy the record and would be a worse defect than the one being
fixed. So the checker must never guess.

Two designs were considered:

  (A) one "CANON" block per doc, everything else declared historical.
      Rejected as the sole mechanism: it guards one line per document
      and leaves the live in-prose claims -- e.g. frame-data-synthesis
      section 13.16's "the overlay is frame-exact on 1,296 of 1,349
      legs", which is the sentence a reader actually acts on --
      completely unguarded. It also asserts something false about the
      other mentions: they are not all historical, they are mostly
      historical.

  (B) an explicit inline marker attached to each individual live
      figure. CHOSEN. Opt-in, so a dated log entry can never be swept
      up by accident; local, so the guard sits on the exact number a
      reader reads; and invisible, because an HTML comment renders as
      nothing in Markdown on GitHub and in every previewer.

Marker form -- the comment follows the number it guards:

    frame-exact on 1,296<!-- canon:pass --> of 1,349<!-- canon:total --> legs

Recognised keys: canon:corpora, canon:total, canon:pass, canon:xfail.
The number may be written with or without a thousands comma; both
`1,296` and `1296` are accepted and compared numerically.

USAGE
-----
    check-canon-numbers.py                 # print the derived canon
    check-canon-numbers.py --check         # + verify every marked figure
    check-canon-numbers.py --check FILE... # verify only these files
    check-canon-numbers.py --list-marks    # show every marked figure found

Exit codes: 0 ok, 1 a marked figure disagrees with the summed truth (or
the golden set is unreadable), 2 usage error.

The last line is always a machine-greppable `CANON SUMMARY:` verdict.
"""

import argparse
import os
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
GOLDEN_DIR = os.path.join(SCRIPT_DIR, "golden")

# Keys a document may mark, mapped to the canon field they assert.
CANON_KEYS = ("corpora", "total", "pass", "xfail")

# `1,296<!-- canon:pass -->` / `**53**<!-- canon:xfail -->`. The number is
# captured immediately before the marker; only whitespace and Markdown
# emphasis/code delimiters may intervene, so a marker can never
# accidentally claim a number from a neighbouring sentence.
MARK_RE = re.compile(
    r"(?P<num>\d{1,3}(?:,\d{3})+|\d+)[*_`\s]*<!--\s*canon:(?P<key>[a-z]+)\s*-->"
)

# Any canon marker at all, so a marker that failed to bind to a number
# (typo, reordered edit) is reported instead of silently ignored.
ANY_MARK_RE = re.compile(r"<!--\s*canon:(?P<key>[a-z]+)\s*-->")

# Documents scanned when --check is given no explicit paths. Read-only.
DEFAULT_SCAN_GLOBS = (
    "docs",
    "tools/frame-data",
)


class CanonError(Exception):
    pass


# ---------------------------------------------------------------------
# Deriving the canon by summing the per-corpus golden tables
# ---------------------------------------------------------------------


def read_golden_table(path):
    """Returns (verdict_counts, n_rows) for one golden TSV.

    The verdict column is located by name from the header row rather
    than by fixed index, so a future golden.py column addition does not
    silently shift what this reads.
    """
    with open(path, "r") as f:
        lines = [ln for ln in f.read().splitlines() if ln != ""]
    if not lines:
        raise CanonError("%s: empty golden file" % path)
    header = lines[0].split("\t")
    if "verdict" not in header:
        raise CanonError(
            "%s: header has no 'verdict' column (got %r)" % (path, header)
        )
    vi = header.index("verdict")
    counts = {}
    for lineno, ln in enumerate(lines[1:], start=2):
        cells = ln.split("\t")
        if len(cells) <= vi:
            raise CanonError(
                "%s:%d: row has %d columns, need at least %d"
                % (path, lineno, len(cells), vi + 1)
            )
        counts[cells[vi]] = counts.get(cells[vi], 0) + 1
    return counts, len(lines) - 1


def derive_canon(golden_dir=GOLDEN_DIR):
    """Sums every golden/*.tsv into the canon. Returns a dict."""
    if not os.path.isdir(golden_dir):
        raise CanonError("golden dir not found: %s" % golden_dir)
    paths = sorted(
        os.path.join(golden_dir, n)
        for n in os.listdir(golden_dir)
        if n.endswith(".tsv")
    )
    if not paths:
        raise CanonError("no golden/*.tsv files under %s" % golden_dir)

    totals = {}
    rows = 0
    per_corpus = []
    for p in paths:
        counts, n = read_golden_table(p)
        rows += n
        for k, v in counts.items():
            totals[k] = totals.get(k, 0) + v
        per_corpus.append((os.path.basename(p)[:-4], n, counts))

    other = {k: v for k, v in totals.items() if k not in ("PASS", "XFAIL")}
    return {
        "corpora": len(paths),
        "total": rows,
        "pass": totals.get("PASS", 0),
        "xfail": totals.get("XFAIL", 0),
        "other": other,
        "per_corpus": per_corpus,
    }


# ---------------------------------------------------------------------
# Finding marked figures in the docs
# ---------------------------------------------------------------------


def iter_scan_files(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, names in os.walk(p):
                dirs[:] = [d for d in dirs if not d.startswith(".")]
                for n in sorted(names):
                    if n.endswith(".md"):
                        yield os.path.join(root, n)
        else:
            yield p


def find_marks(path):
    """Returns (bound, unbound) for one file.

    bound   -- [(lineno, key, value, raw_text)] markers that bound to a number.
    unbound -- [(lineno, key)] markers with no number immediately before them.
    """
    bound = []
    unbound = []
    with open(path, "r", errors="replace") as f:
        for lineno, line in enumerate(f, start=1):
            bound_spans = []
            for m in MARK_RE.finditer(line):
                raw = m.group("num")
                bound.append(
                    (lineno, m.group("key"), int(raw.replace(",", "")), raw)
                )
                bound_spans.append(m.span())
            for m in ANY_MARK_RE.finditer(line):
                if not any(s <= m.start() and m.end() <= e for s, e in bound_spans):
                    unbound.append((lineno, m.group("key")))
    return bound, unbound


# ---------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------


def rel(path):
    try:
        return os.path.relpath(path, REPO_ROOT)
    except ValueError:
        return path


def print_canon(canon, verbose=False):
    print("Frame-data verification canon, summed from %s/*.tsv:" % rel(GOLDEN_DIR))
    print()
    print("  corpora : %d" % canon["corpora"])
    print("  rows    : %d" % canon["total"])
    print("  PASS    : %d" % canon["pass"])
    print("  XFAIL   : %d" % canon["xfail"])
    if canon["other"]:
        print(
            "  OTHER   : %s   <-- non-PASS/XFAIL verdicts present"
            % ", ".join("%s=%d" % kv for kv in sorted(canon["other"].items()))
        )
    print()
    if verbose:
        for name, n, counts in canon["per_corpus"]:
            print(
                "    %-24s %4d rows  %s"
                % (name, n, " ".join("%s=%d" % kv for kv in sorted(counts.items())))
            )
        print()


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Derive the frame-data canon from the golden tables and "
        "check the docs' marked live figures against it.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "--check",
        action="store_true",
        help="verify every canon-marked figure; exit 1 on disagreement",
    )
    ap.add_argument(
        "--list-marks",
        action="store_true",
        help="list every canon-marked figure found, agreeing or not",
    )
    ap.add_argument(
        "--per-corpus",
        action="store_true",
        help="also print the per-corpus row/verdict breakdown that was summed",
    )
    ap.add_argument(
        "--golden-dir",
        default=GOLDEN_DIR,
        help="override the golden table directory (default: %(default)s)",
    )
    ap.add_argument(
        "paths",
        nargs="*",
        help="files or directories to scan (default: docs/ and tools/frame-data/)",
    )
    args = ap.parse_args(argv)

    try:
        canon = derive_canon(args.golden_dir)
    except CanonError as e:
        print("error: %s" % e, file=sys.stderr)
        print(
            "CANON SUMMARY: status=ERROR corpora=? rows=? pass=? xfail=? "
            "marked=0 mismatched=0 unbound=0 files=0"
        )
        return 1

    print_canon(canon, verbose=args.per_corpus)

    if not (args.check or args.list_marks):
        print(
            "CANON SUMMARY: status=DERIVED corpora=%d rows=%d pass=%d xfail=%d "
            "marked=0 mismatched=0 unbound=0 files=0"
            % (canon["corpora"], canon["total"], canon["pass"], canon["xfail"])
        )
        return 0

    scan_roots = args.paths or [os.path.join(REPO_ROOT, g) for g in DEFAULT_SCAN_GLOBS]
    scan_roots = [r for r in scan_roots if os.path.exists(r)]

    marked = 0
    mismatched = 0
    unbound_total = 0
    files_with_marks = set()

    for path in iter_scan_files(scan_roots):
        try:
            bound, unbound = find_marks(path)
        except (IOError, OSError):
            continue
        if not bound and not unbound:
            continue
        files_with_marks.add(path)

        for lineno, key, value, raw in bound:
            marked += 1
            if key not in CANON_KEYS:
                mismatched += 1
                print(
                    "MISMATCH %s:%d  unknown canon key 'canon:%s' "
                    "(known: %s)" % (rel(path), lineno, key, ", ".join(CANON_KEYS))
                )
                continue
            truth = canon[key]
            if value != truth:
                mismatched += 1
                print(
                    "MISMATCH %s:%d  canon:%s says %s, golden tables sum to %d"
                    % (rel(path), lineno, key, raw, truth)
                )
            elif args.list_marks:
                print(
                    "ok       %s:%d  canon:%s = %s"
                    % (rel(path), lineno, key, raw)
                )

        for lineno, key in unbound:
            unbound_total += 1
            print(
                "MISMATCH %s:%d  canon:%s marker has no number immediately "
                "before it" % (rel(path), lineno, key)
            )

    if canon["other"]:
        mismatched += 1
        print(
            "MISMATCH %s  golden tables contain non-PASS/XFAIL verdicts (%s); "
            "the canon's PASS+XFAIL==rows identity no longer holds"
            % (
                rel(args.golden_dir),
                ", ".join("%s=%d" % kv for kv in sorted(canon["other"].items())),
            )
        )

    bad = mismatched + unbound_total
    status = "OK" if bad == 0 else "DRIFT"
    print()
    print(
        "CANON SUMMARY: status=%s corpora=%d rows=%d pass=%d xfail=%d "
        "marked=%d mismatched=%d unbound=%d files=%d"
        % (
            status,
            canon["corpora"],
            canon["total"],
            canon["pass"],
            canon["xfail"],
            marked,
            mismatched,
            unbound_total,
            len(files_with_marks),
        )
    )
    return 1 if (args.check and bad) else 0


if __name__ == "__main__":
    sys.exit(main())
