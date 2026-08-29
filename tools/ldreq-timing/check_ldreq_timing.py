#!/usr/bin/env python3
"""Loader-timing invariance check (task #66).

WHAT IT PROVES
--------------
That the simulation's view of the LDREQ/AFS asset loader does not depend
on how long the disk took -- i.e. that two netplay peers with different
storage speeds compute the same saved state through character select.

The bug this exists for is NOT a rollback bug. AFS_Read is a genuine OS
async read (src/port/io/afs.c:403) drained by AFS_RunServer via
SDL_GetAsyncIOResult (afs.c:313-318), so the frame on which a load
completes is wall-clock. Exit_6th (src/sf33rd/Source/Game/screen/
sel_pl.c:1701-1722) gates the rollback-SAVED Exit_No / Exit_Timer on
Check_PL_Load() + Check_LDREQ_Queue_BG(), so two peers leave character
select on different frames and the match is misaligned from its first
frame.

WHY NOT tools/rollback-determinism
----------------------------------
docs/rollback-determinism-harness.md known limit 9: q_ldreq, rckey_work,
rckey_mmobj, texgrplds, char_init_data, requests, afs and asyncio_queue
are all in that harness's own A1-vs-A2 BASELINE NOISE list -- two
identical no-rollback runs of one binary already disagree about them, so
a divergence here is noise-classified before it can be reported. Its
run-B model also compresses `depth` predicted frames into one real frame,
so the loader's cadence there matches neither run A1 nor production. The
document says outright not to use it to validate a fix in this cluster.

METHOD
------
Vary the wall clock; require the simulation to be invariant. Four runs of
one binary, identical scripted inputs and pinned RNG, differing only in

    --afs-inject-latency-ms   (0 vs N: the "slower peer's disk")
    --ldreq-barrier-force     (the fix, on or off)

Each run writes one CSV row per outer frame (src/test/
ldreq_timing_trace.c) carrying the saved state the loader feeds
(Exit_No / Exit_Timer / G_No / G_Timer) and the loader's own observable
surface (ldreq_result hash, plt_req, head queue slot, Check_PL_Load,
Check_LDREQ_Clear).

VERDICT
-------
  PASS  barrier ON  -> the two latencies produce identical traces, AND
        barrier OFF -> they differ IN THE ROLLBACK-SAVED COLUMNS.
  FAIL  barrier ON  -> traces differ. The fix does not hold.
  ERROR barrier OFF -> the saved columns match. The experiment has no
        signal: the injected latency never propagated into checksummed
        state, so a green ON result would prove nothing.

The control deliberately demands divergence in the SAVED columns
(Exit_No, Exit_Timer, G_No[0..3], G_Timer) and not merely somewhere in
the row. Loader-internal columns move at any injected latency, so a
control keyed on those would pass while the interesting chain -- loader
timing reaching state a desync checksum compares -- stayed unexercised.
That is exactly the shape of a test that cannot fail. Measured on this
tree: at 150 ms the loader columns diverge (105 rows) but Exit_No does
NOT, because Exit_6th happens to sample the gate after both timelines
reconverged; at 400 ms Exit_Timer diverges from frame 318, Exit_No from
319 and G_No[1] from 328 -- one side in the battle (G_No[1]==2) while the
other is still in character select (G_No[1]==1).

Exit 0 = PASS, 1 = FAIL, 2 = harness/plumbing failure or ERROR.
"""

import argparse
import os
import platform
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Character select runs well inside this window on the basic-exchange
# preset: the loader is active around frames 195-260 and Exit_No advances
# around 252-254 on an unloaded host. 600 frames also covers the first
# in-game frames, so a select-phase misalignment shows up as a shifted
# G_No/G_Timer tail rather than only as a local blip.
DEFAULT_FRAMES = 600

# 400 ms, not something smaller, because the control has to reach the
# SAVED columns and 150 ms provably does not on this scene (see the
# module docstring). Also not absurd for the target hardware: the
# character texture group is multi-megabyte and MiSTer reads it from SD.
DEFAULT_LATENCY_MS = 400

BASE_ARGS = ["--test-enable", "--test-pin-rng",
             "--test-scene-preset", "basic-exchange"]

# Column order written by src/test/ldreq_timing_trace.c.
COLUMNS = ["frame", "G_No0", "G_No1", "G_No2", "G_No3", "G_Timer",
           "Exit_No", "Exit_Timer", "SP_No00", "SP_No10",
           "plt_req0", "plt_req1", "ldreq_result_h",
           "head_be", "head_type", "head_rno", "ldreq_break",
           "pl_load", "ldreq_clear"]

# The subset that is in the rollback save set and therefore inside the
# desync checksum: Exit_No/Exit_Timer at game_state.c:471/1210 and
# :622/1361, G_No/G_Timer at :468/:555. Divergence here IS the desync.
SAVED_COLUMNS = ["G_No0", "G_No1", "G_No2", "G_No3", "G_Timer",
                 "Exit_No", "Exit_Timer"]


class HarnessError(Exception):
    pass


def run_game(binary, out_path, log_path, latency_ms, barrier, frames, timeout):
    args = [binary] + BASE_ARGS + [
        "--afs-inject-latency-ms", str(latency_ms),
        "--ldreq-trace", out_path,
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
            raise HarnessError(
                f"game timed out after {timeout}s "
                f"(barrier={int(barrier)} latency={latency_ms}ms, log: {log_path})")

    if proc.returncode != 0:
        raise HarnessError(
            f"game exited {proc.returncode} "
            f"(barrier={int(barrier)} latency={latency_ms}ms, log: {log_path})")

    rows = read_rows(out_path)
    if len(rows) != frames:
        raise HarnessError(
            f"expected {frames} trace rows, got {len(rows)} "
            f"(barrier={int(barrier)} latency={latency_ms}ms, log: {log_path})")
    return rows


def read_rows(path):
    """Rows only: the '#' provenance header and the column header carry the
    run's own configuration and would differ by construction."""
    rows = []
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#") or line.startswith("frame,"):
                continue
            rows.append(line)
    return rows


def first_divergence(a, b):
    """(frame_index, row_a, row_b) of the first differing row, or None."""
    for i, (ra, rb) in enumerate(zip(a, b)):
        if ra != rb:
            return (i, ra, rb)
    if len(a) != len(b):
        i = min(len(a), len(b))
        return (i, a[i] if i < len(a) else "<missing>",
                b[i] if i < len(b) else "<missing>")
    return None


def count_divergent(a, b):
    return sum(1 for ra, rb in zip(a, b) if ra != rb) + abs(len(a) - len(b))


def per_column(a, b):
    """{column: (n_divergent_frames, first_frame, val_a, val_b)}."""
    out = {}
    for ra, rb in zip(a, b):
        fa, fb = ra.split(","), rb.split(",")
        if len(fa) != len(COLUMNS) or len(fb) != len(COLUMNS):
            raise HarnessError(
                f"trace row has {len(fa)}/{len(fb)} fields, expected "
                f"{len(COLUMNS)} — trace format and driver are out of sync")
        for i, col in enumerate(COLUMNS):
            if fa[i] != fb[i]:
                if col in out:
                    n, ff, va, vb = out[col]
                    out[col] = (n + 1, ff, va, vb)
                else:
                    out[col] = (1, fa[0], fa[i], fb[i])
    return out


def describe(label, a, b):
    cols = per_column(a, b)
    div = first_divergence(a, b)
    n = count_divergent(a, b)
    if div is None:
        print(f"  {label}: IDENTICAL over {len(a)} frames")
        return True, cols
    frame, ra, rb = div
    print(f"  {label}: DIVERGED at frame {frame} ({n} of {len(a)} rows differ)")
    print(f"      latency=0   {ra}")
    print(f"      latency=N   {rb}")
    for col in COLUMNS:
        if col in cols:
            cnt, ff, va, vb = cols[col]
            tag = " [SAVED]" if col in SAVED_COLUMNS else ""
            print(f"      {col:16s} {cnt:4d} frames, first {ff}: {va} vs {vb}{tag}")
    return False, cols


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    ap.add_argument("--latency-ms", type=int, default=DEFAULT_LATENCY_MS,
                    help="injected AFS latency for the 'slow peer' run")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--outdir", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if args.latency_ms <= 0:
        print("LDREQ-TIMING SUMMARY: verdict=ERROR reason=latency-ms-must-be-positive")
        return 2

    if not os.access(args.binary, os.X_OK):
        print(f"LDREQ-TIMING SUMMARY: verdict=ERROR reason=binary-not-executable "
              f"path={args.binary}")
        return 2

    outdir = args.outdir or tempfile.mkdtemp(prefix="ldreq-timing-")
    os.makedirs(outdir, exist_ok=True)
    print(f"[ldreq-timing] work dir: {outdir}")
    print(f"[ldreq-timing] frames={args.frames} latency={args.latency_ms}ms "
          f"host={platform.system()}")

    runs = {}
    try:
        for barrier in (False, True):
            for latency in (0, args.latency_ms):
                tag = f"barrier{int(barrier)}_lat{latency}"
                print(f"[ldreq-timing] run {tag} ...")
                runs[(barrier, latency)] = run_game(
                    args.binary,
                    os.path.join(outdir, tag + ".csv"),
                    os.path.join(outdir, tag + ".log"),
                    latency, barrier, args.frames, args.timeout)
    except HarnessError as e:
        print(f"[ldreq-timing] {e}", file=sys.stderr)
        print("LDREQ-TIMING SUMMARY: verdict=ERROR reason=run-failure")
        return 2

    try:
        print("\n[ldreq-timing] barrier OFF (control -- MUST diverge in a SAVED column):")
        off_same, off_cols = describe("barrier=0", runs[(False, 0)],
                                      runs[(False, args.latency_ms)])

        print("[ldreq-timing] barrier ON (the fix -- MUST be identical):")
        on_same, _ = describe("barrier=1", runs[(True, 0)],
                              runs[(True, args.latency_ms)])
    except HarnessError as e:
        print(f"[ldreq-timing] {e}", file=sys.stderr)
        print("LDREQ-TIMING SUMMARY: verdict=ERROR reason=trace-format-mismatch")
        return 2

    off_div = count_divergent(runs[(False, 0)], runs[(False, args.latency_ms)])
    on_div = count_divergent(runs[(True, 0)], runs[(True, args.latency_ms)])
    off_saved = sorted(c for c in off_cols if c in SAVED_COLUMNS)

    print(f"[ldreq-timing] artefacts at {outdir}")

    if not off_saved:
        # No signal in the columns that matter. A green ON result would be
        # vacuous, so never report PASS from here.
        print(f"LDREQ-TIMING SUMMARY: frames={args.frames} latency_ms={args.latency_ms} "
              f"barrier_off_divergent={off_div} barrier_off_saved_divergent=0 "
              f"barrier_on_divergent={on_div} "
              f"verdict=ERROR reason=control-did-not-reach-saved-state")
        return 2

    verdict = "PASS" if on_same else "FAIL"
    print(f"LDREQ-TIMING SUMMARY: frames={args.frames} latency_ms={args.latency_ms} "
          f"barrier_off_divergent={off_div} "
          f"barrier_off_saved_divergent={len(off_saved)} "
          f"barrier_off_saved_columns={','.join(off_saved)} "
          f"barrier_on_divergent={on_div} verdict={verdict}")
    return 0 if on_same else 1


if __name__ == "__main__":
    sys.exit(main())
