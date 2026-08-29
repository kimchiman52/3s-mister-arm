#!/usr/bin/env python3
"""Turn run_matrix.sh JSONL into the NAT matrix table.

The deliverable is not pass/fail -- it is "which pairings connect, how often, and
how long they take". Cells are reported as `connected/attempts` plus the median
time-to-handoff over the successful attempts.

Any row whose joiner did not actually run is counted separately and shown as a
DID-NOT-RUN column, never folded into the failure count: "did not run" and
"ran and failed" are different findings.

Usage: summarize.py results.jsonl [more.jsonl ...]
"""
import json, statistics, sys
from collections import defaultdict

ORDER = ["none", "fullcone", "addr-restricted", "port-restricted", "symmetric"]


def sort_key(t):
    return (ORDER.index(t), t) if t in ORDER else (len(ORDER), t)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    rows = []
    for path in sys.argv[1:]:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        rows.append(json.loads(line))
                    except json.JSONDecodeError as e:
                        sys.stderr.write("skipping unparseable row: %s\n" % e)

    if not rows:
        print("no rows")
        return 1

    by_label = defaultdict(list)
    for r in rows:
        by_label[r.get("label", "default")].append(r)

    for label, lrows in by_label.items():
        cells = defaultdict(list)
        for r in lrows:
            cells[(r.get("natA", "?"), r.get("natB", "?"))].append(r)

        a_types = sorted({a for a, _ in cells}, key=sort_key)
        b_types = sorted({b for _, b in cells}, key=sort_key)

        sample = lrows[0]
        print("\n## %s" % label)
        print("conditions: owd_a=%sms owd_b=%sms loss=%s%% deliver_loss=%s%%"
              % (sample.get("owd_a", 0), sample.get("owd_b", 0),
                 sample.get("loss", 0), sample.get("deliver_loss", 0)))
        print()

        # Flag any cell whose emulation did not produce the declared NAT type.
        #
        # "none" (an un-NATted, publicly routable host) is BEHAVIOURALLY a full
        # cone: endpoint-independent in both mapping and filtering. The RFC 4787
        # classifier cannot distinguish the two, and should not -- to a peer they
        # are the same thing. So that pairing is an accepted equivalence, not a
        # mis-emulation.
        EQUIV = {("none", "fullcone")}
        mismatches = []
        for (a, b), rs in cells.items():
            for r in rs:
                ma, mb = r.get("measA"), r.get("measB")
                if (ma and ma not in (a, "unmeasured")
                        and (a, ma) not in EQUIV and (a, ma) not in mismatches):
                    mismatches.append((a, ma))
                if (mb and mb not in (b, "unmeasured")
                        and (b, mb) not in EQUIV and (b, mb) not in mismatches):
                    mismatches.append((b, mb))

        head = "| host A \\ joiner B | " + " | ".join(b_types) + " |"
        print(head)
        print("|" + "---|" * (len(b_types) + 1))
        for a in a_types:
            out = ["| %s " % a]
            for b in b_types:
                rs = cells.get((a, b), [])
                if not rs:
                    out.append("| - ")
                    continue
                ran = [r for r in rs if r.get("cell_status") != "DID_NOT_RUN"]
                ok = [r for r in ran if r.get("cell_status") == "CONNECTED"]
                times = []
                for r in ok:
                    j = r.get("join") or {}
                    if isinstance(j, dict) and j.get("ms_to_handoff"):
                        times.append(j["ms_to_handoff"])
                cellstr = "%d/%d" % (len(ok), len(ran))
                if times:
                    cellstr += " %.1fs" % (statistics.median(times) / 1000.0)
                didnt = len(rs) - len(ran)
                if didnt:
                    cellstr += " (!%d did-not-run)" % didnt
                out.append("| %s " % cellstr)
            print("".join(out) + "|")

        print("\nlegend: connected/attempts, median time-to-handoff over successes")

        if mismatches:
            print("\n**Emulation mismatches (declared -> measured):**")
            for d, m in mismatches:
                print("- declared `%s` measured `%s`" % (d, m))
        else:
            print("\nAll cells: measured NAT type matched the declared type.")

        # Failure-mode breakdown -- which terminal state the joiner reached.
        modes = defaultdict(int)
        for r in lrows:
            if r.get("cell_status") in ("NOT_CONNECTED", "TIMEOUT"):
                j = r.get("join") or {}
                st = j.get("final_state", "?") if isinstance(j, dict) else "?"
                modes["%s/%s/%s" % (r.get("natA"), r.get("natB"), st)] += 1
        if modes:
            print("\n**Failure modes (natA/natB/final joiner state -> count):**")
            for k in sorted(modes):
                print("- %s -> %d" % (k, modes[k]))

    return 0


if __name__ == "__main__":
    sys.exit(main())
