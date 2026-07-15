#!/usr/bin/env python3
"""Diff a frame_trace.c trace log against tools/frame-data/compile_corpus.py's
expected.json oracle.

See docs/plan-frame-data-harness.md section 1.7 (Step H5) for the spec.
Usage: python3 check_frame_data.py <trace.log> <expected.json>
Exit code: 0 iff every entry is PASS, XFAIL (known-open), or explicitly
unchecked; nonzero if anything is FAIL, XPASS, NO-DATA, or SHAPE.

Comparison rule: when a window contains more than one FINAL line, only the
FIRST FINAL is compared against expect.{S,A,R,adv,outcome} - later FINALs in
the same window are only checked against the entry's expected count (see
`finals` below), never against the numeric fields.

outcome: NONE (plan section 1.8 item 5, negative controls - taunt/throw/jump
entries where nothing should finalize) expects ZERO FINAL lines in the
window; any FINAL present is a mismatch (reported with what finalized).
This is the one outcome for which an empty window is success rather than
NO-DATA.

finals: N (expected.json, default 1, see compile_corpus.py's `finals` corpus
field) is the expected FINAL-line count for outcomes other than NONE. A
window with a different count is a mismatch even if the first FINAL's
fields match - this is what lets a chain/merge xfail entry (finals: 1,
currently emitting extra FINALs from a merge bug) flip to XPASS once a fix
makes the engine emit exactly the expected count again.
"""

import json
import re
import sys

# "kd" (knockdown flag, Phase 4 item 3) is compared like the other numeric
# fields (absent expectations skipped) but, unlike S/A/R/adv, is NOT shown
# in the got column unless the entry's expect.* actually asserts it - see
# the exp-gated filter in format_actual - so unrelated entries' got columns
# stay byte-identical even though every FINAL line now carries `kd=N`.
NUMERIC_FIELDS = ("S", "A", "R", "adv", "kd")

SCRIPT_RE = re.compile(r"^# F=(\d+) SCRIPT (.*)$")
FINAL_RE = re.compile(r"^# F=(\d+) FINAL (.*)$")
KV_RE = re.compile(r"(\w+)=([+-]?\w+)")


def parse_kv(rest):
    return {m.group(1): m.group(2) for m in KV_RE.finditer(rest)}


def parse_trace(trace_path):
    """Returns dict[label] -> list of FINAL kv-dicts, in file order, for
    every `# F=n SCRIPT <label>` window. A window runs from its SCRIPT line
    to the next SCRIPT line (or EOF); FINAL lines in between belong to it."""
    windows = {}
    current_label = None
    current_finals = []

    def flush():
        if current_label is not None:
            windows.setdefault(current_label, [])
            windows[current_label].extend(current_finals)

    with open(trace_path, "r") as f:
        for line in f:
            line = line.rstrip("\n")
            m = SCRIPT_RE.match(line)
            if m:
                flush()
                current_label = m.group(2).strip()
                current_finals = []
                continue
            m = FINAL_RE.match(line)
            if m and current_label is not None:
                current_finals.append(parse_kv(m.group(2)))
        flush()

    return windows


def format_expected(exp):
    parts = [f"outcome={exp['outcome']}"]
    for field in NUMERIC_FIELDS:
        if exp.get(field) is not None:
            parts.append(f"{field}={exp[field]:+d}" if field == "adv" else f"{field}={exp[field]}")
    if exp.get("finals", 1) != 1:
        parts.append(f"finals={exp['finals']}")
    if exp.get("xfail"):
        parts.append(f"[xfail: {exp['xfail']}]")
    return " ".join(parts)


def format_actual(final, exp=None):
    """`exp` gates the "kd" column only: kd=0/1 is present on every FINAL
    line unconditionally (unlike S/A/R/adv, which vary move-to-move), so
    without this filter every row's got column would grow a `kd=` entry
    regardless of whether the corpus entry expects one - printing it only
    when `exp` actually declares expect.kd keeps unrelated entries' got
    columns byte-identical to pre-kd-tooling baselines."""
    if final is None:
        return "(no data)"
    parts = [f"outcome={final.get('outcome', '?')}"]
    for field in NUMERIC_FIELDS:
        if field == "kd" and (exp is None or exp.get("kd") is None):
            continue
        if field in final:
            parts.append(f"{field}={final[field]}")
    return " ".join(parts)


def is_shape_mismatch(expected_outcome, actual_outcome):
    if expected_outcome == actual_outcome:
        return False
    return (expected_outcome in ("HIT", "BLOCK") and actual_outcome == "WHIFF") or (
        expected_outcome == "WHIFF" and actual_outcome in ("HIT", "BLOCK")
    )


def evaluate_entry(label, exp, finals):
    """Returns (verdict, got_str, mismatch_notes)."""
    xfail_reason = exp.get("xfail")

    if exp["outcome"] == "NONE":
        return _evaluate_none_entry(exp, finals, xfail_reason)

    if not finals:
        return ("NO-DATA", "(no data)", ["no FINAL in window"])

    first = finals[0]
    expected_finals = exp.get("finals", 1)
    extra = len(finals) - expected_finals
    actual_outcome = first.get("outcome", "NONE")

    notes = []
    if actual_outcome != exp["outcome"]:
        notes.append(f"outcome {actual_outcome} != {exp['outcome']}")
    for field in NUMERIC_FIELDS:
        if exp.get(field) is None:
            continue
        try:
            actual_val = int(first[field])
        except (KeyError, ValueError):
            notes.append(f"{field} missing/unparseable in trace")
            continue
        if actual_val != exp[field]:
            notes.append(f"{field} {actual_val} != {exp[field]}")
    if extra > 0:
        notes.append(f"{extra} extra FINAL(s) in window (expected {expected_finals})")
    elif extra < 0:
        notes.append(f"only {len(finals)} FINAL(s) in window (expected {expected_finals})")

    got_str = format_actual(first, exp)
    if extra != 0:
        got_str += f" ({len(finals)} FINAL(s) total)"

    shape = is_shape_mismatch(exp["outcome"], actual_outcome)
    matched = not notes

    if xfail_reason:
        if matched:
            verdict = "XPASS"
        else:
            verdict = "XFAIL"
    else:
        if matched:
            verdict = "PASS"
        elif shape:
            verdict = "SHAPE"
        else:
            verdict = "FAIL"

    return (verdict, got_str, notes)


def _evaluate_none_entry(exp, finals, xfail_reason):
    """outcome: NONE entries (negative controls) expect zero FINALs in the
    window - any FINAL present is itself the mismatch, reported with what
    unexpectedly finalized."""
    if not finals:
        matched = True
        notes = []
        got_str = "(no data)"
    else:
        matched = False
        outcomes_seen = ", ".join(f.get("outcome", "?") for f in finals)
        notes = [f"{len(finals)} unexpected FINAL(s): {outcomes_seen}"]
        got_str = format_actual(finals[0], exp)
        if len(finals) > 1:
            got_str += f" (+{len(finals) - 1} more)"

    if xfail_reason:
        verdict = "XPASS" if matched else "XFAIL"
    else:
        verdict = "PASS" if matched else "FAIL"

    return (verdict, got_str, notes)


FAILING_VERDICTS = {"FAIL", "XPASS", "NO-DATA", "SHAPE"}


def main(argv):
    if len(argv) != 2:
        print("usage: check_frame_data.py <trace.log> <expected.json>", file=sys.stderr)
        return 2
    trace_path, expected_path = argv

    with open(expected_path, "r") as f:
        expected = json.load(f)
    windows = parse_trace(trace_path)

    rows = []
    for label, exp in expected.items():
        finals = windows.get(label, [])
        verdict, got_str, notes = evaluate_entry(label, exp, finals)
        exp_str = format_expected(exp)
        if notes and verdict not in ("PASS",):
            got_str = got_str + "  [" + "; ".join(notes) + "]"
        rows.append((label, exp_str, got_str, verdict))

    label_w = max((len(r[0]) for r in rows), default=5)
    exp_w = max((len(r[1]) for r in rows), default=8)
    got_w = max((len(r[2]) for r in rows), default=3)

    label_w = max(label_w, len("label"))
    exp_w = max(exp_w, len("expected"))
    got_w = max(got_w, len("got"))

    header = f"{'label':<{label_w}}  {'expected':<{exp_w}}  {'got':<{got_w}}  verdict"
    print(header)
    print("-" * len(header))
    for label, exp_str, got_str, verdict in rows:
        print(f"{label:<{label_w}}  {exp_str:<{exp_w}}  {got_str:<{got_w}}  {verdict}")

    counts = {}
    for row in rows:
        counts[row[3]] = counts.get(row[3], 0) + 1

    print()
    summary = ", ".join(f"{k}={v}" for k, v in sorted(counts.items()))
    print(f"total={len(rows)} ({summary})")

    fail_count = sum(counts.get(v, 0) for v in FAILING_VERDICTS)
    return 1 if fail_count > 0 else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
