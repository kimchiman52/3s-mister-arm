#!/usr/bin/env python3
"""Golden-file verification for the frame-data harness.

Reuses check_frame_data.py's parser (parse_trace/evaluate_entry/
NUMERIC_FIELDS) rather than reinventing trace parsing - see
tools/frame-data/CORPUS-AUTHORING.md and the tooling plan for rationale.

Golden format: a normalized, per-entry TSV, one row per expected.json
label (in expected.json insertion order == corpus order, deterministic).
Columns (tab-separated, header row):

    label   verdict   outcome   n   S   A   R   adv   kd

- verdict: the SAME verdict check_frame_data.evaluate_entry computes
  (PASS/XFAIL/FAIL/XPASS/NO-DATA/SHAPE).
- n: len(finals), the count of FINAL lines in the entry's window. This is
  a discriminator the checker itself uses (evaluate_entry compares
  len(finals) against exp["finals"]) - storing it means a FINAL-count
  drift that leaves verdict + finals[0] unchanged (e.g. an xfail entry
  emitting 2->3 FINALs) still shows up as golden drift instead of hiding.
- outcome,S,A,R,adv,kd: the *measured* values read from the first FINAL
  of the entry's window (finals[0]) - the "got" side. "-" when absent
  (e.g. NO-DATA / NONE windows have no FINAL -> n=0, all measured "-").

Deliberately NOT stored: the free-text xfail reason (re-wording it must
NOT drift the golden) and the raw checker table (column widths reflow
whenever any label/expected string's length changes, which would
false-positive unrelated rows - see check_frame_data.py:219-231).

Usage:
    golden.py emit <trace.log> <expected.json>
        -> writes the normalized TSV to stdout.
    golden.py check <golden.tsv> <trace.log> <expected.json>
        -> recomputes rows from trace.log/expected.json, diffs them
           against golden.tsv, prints per-entry drift, exit 0 iff
           identical (no drift), else nonzero.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_frame_data as C

COLUMNS = ("label", "verdict", "outcome", "n", "S", "A", "R", "adv", "kd")
MEASURED_FIELDS = ("S", "A", "R", "adv", "kd")


def golden_rows(trace_path, expected_path):
    """Returns a list of rows (one per expected.json label, in insertion
    order), each a list matching COLUMNS: [label, verdict, outcome, n,
    S, A, R, adv, kd]."""
    with open(expected_path, "r") as f:
        exp_map = json.load(f)
    windows = C.parse_trace(trace_path)

    rows = []
    for label, exp in exp_map.items():
        finals = windows.get(label, [])
        verdict, _got, _notes = C.evaluate_entry(label, exp, finals)
        first = finals[0] if finals else {}
        outcome = first.get("outcome", "-")
        nums = [str(first[f]) if f in first else "-" for f in MEASURED_FIELDS]
        rows.append([label, verdict, outcome, str(len(finals))] + nums)
    return rows


def format_tsv(rows):
    lines = ["\t".join(COLUMNS)]
    for row in rows:
        lines.append("\t".join(row))
    return "\n".join(lines) + "\n"


def parse_tsv(text):
    """Parses a golden TSV (as produced by format_tsv) into a list of
    rows matching COLUMNS. Tolerates a trailing newline / blank lines."""
    lines = [line for line in text.splitlines() if line != ""]
    if not lines:
        return []
    header = lines[0].split("\t")
    if tuple(header) != COLUMNS:
        raise ValueError(f"unexpected golden header: {header!r} (expected {COLUMNS!r})")
    rows = []
    for line in lines[1:]:
        rows.append(line.split("\t"))
    return rows


def cmd_emit(argv):
    if len(argv) != 2:
        print("usage: golden.py emit <trace.log> <expected.json>", file=sys.stderr)
        return 2
    trace_path, expected_path = argv
    rows = golden_rows(trace_path, expected_path)
    sys.stdout.write(format_tsv(rows))
    return 0


def diff_rows(golden_rows_list, current_rows_list):
    """Returns a list of human-readable drift-report lines; empty list
    means no drift. Compares by label so added/removed entries and
    per-field changes on shared entries are both reported."""
    golden_by_label = {r[0]: r for r in golden_rows_list}
    current_by_label = {r[0]: r for r in current_rows_list}

    lines = []

    # Preserve golden order for shared/removed labels, then append any
    # new labels (present only in the current run) at the end.
    golden_labels = [r[0] for r in golden_rows_list]
    seen = set()
    ordered_labels = []
    for label in golden_labels:
        if label not in seen:
            ordered_labels.append(label)
            seen.add(label)
    for label in (r[0] for r in current_rows_list):
        if label not in seen:
            ordered_labels.append(label)
            seen.add(label)

    for label in ordered_labels:
        g = golden_by_label.get(label)
        c = current_by_label.get(label)
        if g is None:
            lines.append(f"DRIFT +{label}   (row present in run, absent in golden)")
            continue
        if c is None:
            lines.append(f"DRIFT -{label}   (row present in golden, absent in run)")
            continue
        if g == c:
            continue
        field_diffs = []
        for col, gval, cval in zip(COLUMNS[1:], g[1:], c[1:]):
            if gval != cval:
                field_diffs.append(f"{col} {gval}->{cval}")
        lines.append(f"DRIFT {label}   " + "   ".join(field_diffs))

    return lines


def cmd_check(argv):
    if len(argv) != 3:
        print("usage: golden.py check <golden.tsv> <trace.log> <expected.json>", file=sys.stderr)
        return 2
    golden_path, trace_path, expected_path = argv

    with open(golden_path, "r") as f:
        golden_text = f.read()
    golden_rows_list = parse_tsv(golden_text)
    current_rows_list = golden_rows(trace_path, expected_path)

    drift_lines = diff_rows(golden_rows_list, current_rows_list)
    if drift_lines:
        for line in drift_lines:
            print(line)
        return 1

    print(f"golden OK ({len(current_rows_list)} entries)")
    return 0


def main(argv):
    if not argv:
        print(__doc__, file=sys.stderr)
        return 2
    sub, rest = argv[0], argv[1:]
    if sub == "emit":
        return cmd_emit(rest)
    if sub == "check":
        return cmd_check(rest)
    print(f"error: unknown subcommand {sub!r} (expected emit|check)", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
