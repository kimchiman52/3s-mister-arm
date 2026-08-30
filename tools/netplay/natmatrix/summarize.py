#!/usr/bin/env python3
"""Turn run_matrix.sh JSONL into the NAT matrix table.

The deliverable is not pass/fail -- it is "which pairings connect, how often, and
how long they take". Cells are reported as `connected/attempts` plus the median
time-to-handoff over the successful attempts.

Any row whose joiner did not actually run is counted separately and shown as a
DID-NOT-RUN column, never folded into the failure count: "did not run" and
"ran and failed" are different findings.

RIG ERRORS ARE NOT DATA. `TOPOLOGY_UP_FAILED` and `RIG_ERROR` rows used to land
in the denominator of every cell, and because such rows carry no measA/measB the
emulation check saw nothing to compare and printed "All cells: measured NAT type
matched the declared type." A run in which the namespaces never came up and not
one packet was sent therefore rendered as a clean NAT finding. That is a false
pass, so this script now REFUSES to summarize any file set containing rig-error
rows and exits non-zero; pass --force to see the (contaminated) grid anyway, and
it still exits non-zero.

Exit codes:
    0  summarized a clean run
    1  no rows / no usable rows
    2  bad usage
    5  run contains rig-error rows -- refused (or forced, but still refused)

Usage: summarize.py [--force] results.jsonl [more.jsonl ...]
"""
import json, statistics, sys
from collections import defaultdict

ORDER = ["none", "fullcone", "addr-restricted", "port-restricted", "symmetric"]

# Statuses that represent a real measured attempt. Anything else is either a rig
# failure or an unknown status the writer added without teaching this reader --
# both of which must be excluded from the denominator, never silently scored.
SCORABLE = {"CONNECTED", "NOT_CONNECTED", "TIMEOUT"}
RIG_STATUSES = {"TOPOLOGY_UP_FAILED", "RIG_ERROR"}


def sort_key(t):
    return (ORDER.index(t), t) if t in ORDER else (len(ORDER), t)


def classify(status):
    if status in SCORABLE:
        return "scored"
    if status == "DID_NOT_RUN":
        return "did_not_run"
    return "rig"          # RIG_ERROR, TOPOLOGY_UP_FAILED, or anything unknown


def main():
    argv = [a for a in sys.argv[1:]]
    force = False
    if "--force" in argv:
        force = True
        argv.remove("--force")
    if not argv:
        print(__doc__)
        return 2

    rows = []
    for path in argv:
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

    # ---- refuse-before-scoring gate ---------------------------------------
    rig_rows = [r for r in rows if classify(r.get("cell_status")) == "rig"]
    if rig_rows:
        counts = defaultdict(int)
        for r in rig_rows:
            counts[(r.get("label", "?"), r.get("natA", "?"), r.get("natB", "?"),
                    r.get("cell_status", "?"))] += 1
        print("## REFUSING TO SUMMARIZE")
        print()
        print("%d of %d rows are rig errors, not measurements. Scoring them would"
              % (len(rig_rows), len(rows)))
        print("turn a run that measured nothing into a NAT finding. Affected:")
        print()
        for (lab, a, b, st), n in sorted(counts.items()):
            print("- `%s` %s x %s: %s x%d" % (lab, a, b, st, n))
        print()
        if not force:
            print("Fix the rig and re-run. Pass --force to print the contaminated")
            print("grid anyway (still exits non-zero).")
            return 5

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
        # A row with no measurement at all (rig-error rows carry none; a
        # classifier that timed out records "unmeasured") is an UNVERIFIED cell,
        # not a matching one. Tracking these separately is what stops the
        # "All cells matched" line from being printed over a run that measured
        # nothing.
        unverified = []
        for (a, b), rs in cells.items():
            for r in rs:
                if classify(r.get("cell_status")) == "rig":
                    if (a, b) not in unverified:
                        unverified.append((a, b))
                    continue
                ma, mb = r.get("measA"), r.get("measB")
                for decl, meas in ((a, ma), (b, mb)):
                    if not meas or meas == "unmeasured":
                        if (a, b) not in unverified:
                            unverified.append((a, b))
                    elif (meas != decl and (decl, meas) not in EQUIV
                            and (decl, meas) not in mismatches):
                        mismatches.append((decl, meas))

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
                # Only SCORABLE rows form the denominator. Rig errors and
                # did-not-run rows are reported beside the fraction, never
                # inside it.
                ran = [r for r in rs
                       if classify(r.get("cell_status")) == "scored"]
                ok = [r for r in ran if r.get("cell_status") == "CONNECTED"]
                rig = [r for r in rs if classify(r.get("cell_status")) == "rig"]
                didnt = [r for r in rs
                         if classify(r.get("cell_status")) == "did_not_run"]
                times = []
                for r in ok:
                    j = r.get("join") or {}
                    if isinstance(j, dict) and j.get("ms_to_handoff"):
                        times.append(j["ms_to_handoff"])
                if ran:
                    cellstr = "%d/%d" % (len(ok), len(ran))
                    if times:
                        cellstr += " %.1fs" % (statistics.median(times) / 1000.0)
                else:
                    cellstr = "NO DATA"
                if didnt:
                    cellstr += " (!%d did-not-run)" % len(didnt)
                if rig:
                    cellstr += " (!%d RIG-ERROR)" % len(rig)
                out.append("| %s " % cellstr)
            print("".join(out) + "|")

        print("\nlegend: connected/attempts, median time-to-handoff over successes")

        if mismatches:
            print("\n**Emulation mismatches (declared -> measured):**")
            for d, m in mismatches:
                print("- declared `%s` measured `%s`" % (d, m))
        if unverified:
            print("\n**Cells with NO NAT-type measurement (unverified emulation):**")
            for a, b in unverified:
                print("- %s x %s" % (a, b))
        if not mismatches and not unverified:
            print("\nAll cells: measured NAT type matched the declared type.")
        elif not mismatches:
            print("\nNo mismatch was observed, but the emulation of the cells "
                  "listed above was never verified -- this is NOT a clean run.")

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

    # Reached only under --force (the un-forced path returned 5 above).
    return 5 if rig_rows else 0


if __name__ == "__main__":
    sys.exit(main())
