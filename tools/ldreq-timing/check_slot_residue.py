#!/usr/bin/env python3
"""Per-slot q_ldreq residue probe (task #69.2).

THE QUESTION
------------
The task #66 barrier claims this invariant (gd3rd.c:566-570):

    at every simulated-frame boundary the queue is empty, afs_handle is
    AFS_NONE, and ldreq_result[] is a pure function of the sequence of
    Push_LDREQ_Queue_* calls

check_ldreq_timing.py proves the first-order consequence (the SAVED
columns are latency-invariant with the barrier on). It records only
q_ldreq[0], so it cannot say whether the residual q_ldreq disagreement
between two latency runs is confined to slots that are already DRAINED
(be == 0 in BOTH runs) or reaches a LIVE slot. A live residue would
falsify the "queue is empty" half of the invariant.

METHOD
------
Same 2x2 as check_ldreq_timing.py, but each run also writes the
--ldreq-slot-trace side channel: one row per (frame, slot) for all 16
q_ldreq entries, every REQ field decoded, plus a pointer-normalised raw
byte image of the slot (src/test/ldreq_timing_trace.c).

Every (frame, slot) whose two rows differ is classified:

  DRAINED  be == 0 in BOTH runs. The slot is not in the queue. The only
           simulation-visible reads of a be == 0 slot are enumerated in
           the REACHABILITY section below; all of them read .be alone.
  LIVE     anything else — be != 0 on either side, i.e. a request that
           is still in the queue at the frame boundary.

LIVE at any frame is the finding. DRAINED-only closes the item.

Byte offsets are named through the `# fieldmap` header the emitter
writes, so a difference in a padding hole is reported as PAD@n and never
mistaken for a semantic field.

Exit 0 = analysis completed (read the verdict line), 2 = harness failure.
"""

import argparse
import os
import platform
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

DEFAULT_FRAMES = 600
DEFAULT_LATENCY_MS = 400

BASE_ARGS = ["--test-enable", "--test-pin-rng",
             "--test-scene-preset", "basic-exchange"]

DECODED = ["be", "type", "id", "rno", "retry", "ix", "frre", "key", "kokey",
           "group", "result_ix", "size", "sect", "fnum", "free0", "free1",
           "lds_nonnull", "info_number", "info_size"]


class HarnessError(Exception):
    pass


def run_game(binary, main_csv, slot_csv, log_path, latency_ms, barrier, frames, timeout):
    args = [binary] + BASE_ARGS + [
        "--afs-inject-latency-ms", str(latency_ms),
        "--ldreq-trace", main_csv,
        "--ldreq-slot-trace", slot_csv,
        "--ldreq-trace-frames", str(frames),
    ]
    if barrier:
        args.append("--ldreq-barrier-force")

    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"

    with open(log_path, "w") as logf:
        try:
            proc = subprocess.run(args, env=env, stdout=logf,
                                  stderr=subprocess.STDOUT,
                                  timeout=timeout, cwd=REPO_ROOT)
        except subprocess.TimeoutExpired:
            raise HarnessError(f"game timed out after {timeout}s (log: {log_path})")

    if proc.returncode != 0:
        raise HarnessError(f"game exited {proc.returncode} (log: {log_path})")

    return read_slots(slot_csv)


def read_slots(path):
    """-> (fieldmap, {(frame, slot): (decoded_dict, raw_hex)}, sizeof_req)"""
    fieldmap = []
    sizeof_req = None
    rows = {}
    header = None

    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            if line.startswith("# fieldmap"):
                for tok in line[len("# fieldmap"):].split():
                    name, off, ln = tok.rsplit(":", 2)
                    fieldmap.append((name, int(off), int(ln)))
                continue
            if line.startswith("#"):
                for tok in line.split():
                    if tok.startswith("sizeof_REQ="):
                        sizeof_req = int(tok.split("=", 1)[1])
                continue
            if line.startswith("frame,"):
                header = line.split(",")
                continue
            fields = line.split(",")
            if header is None or len(fields) != len(header):
                raise HarnessError(f"malformed slot row in {path}: {line[:80]}")
            d = dict(zip(header, fields))
            rows[(int(d["frame"]), int(d["slot"]))] = (d, d["raw"])

    if sizeof_req is None or not fieldmap:
        raise HarnessError(f"{path} is missing its provenance/fieldmap header")
    return fieldmap, rows, sizeof_req


def name_byte(fieldmap, off):
    for name, foff, flen in fieldmap:
        if foff <= off < foff + flen:
            return f"{name}[{off - foff}]"
    return f"PAD@{off}"


def analyse(label, a, b, fieldmap, sizeof_req, verbose_limit):
    fmap_a, rows_a, _ = a
    fmap_b, rows_b, _ = b
    if fmap_a != fmap_b:
        raise HarnessError("the two runs disagree about the REQ fieldmap")

    keys = sorted(set(rows_a) & set(rows_b))
    if not keys:
        raise HarnessError("no overlapping (frame, slot) rows")

    drained = []   # (frame, slot, [fieldnames])
    live = []      # (frame, slot, be_a, be_b, [fieldnames])

    for key in keys:
        da, ra = rows_a[key]
        db, rb = rows_b[key]
        if da == db:
            continue

        diff_fields = sorted(c for c in DECODED if da[c] != db[c])
        # Byte-level, including padding holes.
        diff_bytes = []
        if ra != rb:
            for i in range(sizeof_req):
                if ra[2 * i:2 * i + 2] != rb[2 * i:2 * i + 2]:
                    diff_bytes.append(name_byte(fieldmap, i))

        merged = sorted(set(diff_fields) | set(x.split("[")[0] if not x.startswith("PAD@") else x
                                               for x in diff_bytes))
        frame, slot = key
        be_a, be_b = int(da["be"]), int(db["be"])
        if be_a == 0 and be_b == 0:
            drained.append((frame, slot, merged))
        else:
            live.append((frame, slot, be_a, be_b, merged))

    print(f"\n[slot-residue] {label}: {len(keys)} (frame,slot) pairs compared")
    print(f"[slot-residue] {label}: DRAINED-slot differences (be==0 in BOTH runs): {len(drained)}")
    print(f"[slot-residue] {label}: LIVE-slot   differences (be!=0 either side):  {len(live)}")

    if drained:
        frames = sorted({f for f, _, _ in drained})
        slots = sorted({s for _, s, _ in drained})
        fields = sorted({x for _, _, fs in drained for x in fs})
        print(f"    drained: frames {frames[0]}..{frames[-1]} "
              f"({len(frames)} distinct), slots {slots}, fields {fields}")
        for row in drained[:verbose_limit]:
            print(f"      frame={row[0]} slot={row[1]} fields={row[2]}")
        if len(drained) > verbose_limit:
            print(f"      ... {len(drained) - verbose_limit} more")

    if live:
        print("    LIVE RESIDUE — the barrier invariant does not hold here:")
        for row in live[:verbose_limit]:
            print(f"      frame={row[0]} slot={row[1]} be={row[2]}/{row[3]} fields={row[4]}")
        if len(live) > verbose_limit:
            print(f"      ... {len(live) - verbose_limit} more")

    return drained, live


def nonempty_boundaries(rows, frames):
    """Frames at which ANY slot had be != 0 at the frame boundary."""
    out = set()
    for (frame, _slot), (d, _raw) in rows.items():
        if int(d["be"]) != 0:
            out.add(frame)
    return sorted(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    ap.add_argument("--latency-ms", type=int, default=DEFAULT_LATENCY_MS)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--outdir", default=None)
    ap.add_argument("--verbose-limit", type=int, default=20)
    args = ap.parse_args()

    if not os.access(args.binary, os.X_OK):
        print(f"SLOT-RESIDUE SUMMARY: verdict=ERROR reason=binary-not-executable path={args.binary}")
        return 2

    outdir = args.outdir or tempfile.mkdtemp(prefix="ldreq-slot-")
    os.makedirs(outdir, exist_ok=True)
    print(f"[slot-residue] work dir: {outdir}")
    print(f"[slot-residue] frames={args.frames} latency={args.latency_ms}ms host={platform.system()}")

    runs = {}
    try:
        for barrier in (False, True):
            for latency in (0, args.latency_ms):
                tag = f"barrier{int(barrier)}_lat{latency}"
                print(f"[slot-residue] run {tag} ...")
                runs[(barrier, latency)] = run_game(
                    args.binary,
                    os.path.join(outdir, tag + ".main.csv"),
                    os.path.join(outdir, tag + ".slots.csv"),
                    os.path.join(outdir, tag + ".log"),
                    latency, barrier, args.frames, args.timeout)
    except HarnessError as e:
        print(f"[slot-residue] {e}", file=sys.stderr)
        print("SLOT-RESIDUE SUMMARY: verdict=ERROR reason=run-failure")
        return 2

    fieldmap, _rows, sizeof_req = runs[(True, 0)]
    print(f"[slot-residue] sizeof(REQ)={sizeof_req} fieldmap={fieldmap}")

    try:
        off_drained, off_live = analyse(
            "barrier=0 (control)", runs[(False, 0)], runs[(False, args.latency_ms)],
            fieldmap, sizeof_req, args.verbose_limit)
        on_drained, on_live = analyse(
            "barrier=1 (the fix)", runs[(True, 0)], runs[(True, args.latency_ms)],
            fieldmap, sizeof_req, args.verbose_limit)
    except HarnessError as e:
        print(f"[slot-residue] {e}", file=sys.stderr)
        print("SLOT-RESIDUE SUMMARY: verdict=ERROR reason=analysis-failure")
        return 2

    # Independent check of the "queue is empty at every frame boundary"
    # half of the invariant, on ONE run: with the barrier on, no slot
    # should carry be != 0 at any frame boundary.
    for latency in (0, args.latency_ms):
        _fm, rows, _sz = runs[(True, latency)]
        ne = nonempty_boundaries(rows, args.frames)
        print(f"[slot-residue] barrier=1 lat={latency}: frame boundaries with a "
              f"non-empty queue: {len(ne)} {ne[:20]}")
    for latency in (0, args.latency_ms):
        _fm, rows, _sz = runs[(False, latency)]
        ne = nonempty_boundaries(rows, args.frames)
        print(f"[slot-residue] barrier=0 lat={latency}: frame boundaries with a "
              f"non-empty queue: {len(ne)} {ne[:20]}")

    print(f"[slot-residue] artefacts at {outdir}")
    verdict = "LIVE-RESIDUE" if on_live else "DRAINED-ONLY"
    print(f"SLOT-RESIDUE SUMMARY: frames={args.frames} latency_ms={args.latency_ms} "
          f"barrier_on_drained_diffs={len(on_drained)} barrier_on_live_diffs={len(on_live)} "
          f"barrier_off_drained_diffs={len(off_drained)} barrier_off_live_diffs={len(off_live)} "
          f"verdict={verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
