#!/usr/bin/env python3
"""Rollback-observability probe for the loader surface (task #69).

WHAT THIS IS FOR. The rollback-determinism harness answers "did this symbol's
bytes change?", which for the LDREQ/AFS cluster is the wrong question — known
limit 9 in docs/rollback-determinism-harness.md explains why. The question
that actually decides a desync is "did anything the simulation READS change,
and did it change at a frame where the simulation reads it?". This probe
answers that directly, for the one member of the cluster the gate still
reported: plt_req.

HOW. src/test/ldreq_timing_trace.c already emits, once per real frame, both
the loader surface (plt_req0/1, ldreq_result_h, head_be/type/rno, ldreq_break)
and the exact expressions the simulation gates saved state on
(pl_load = Check_PL_Load(), ldreq_clear = Check_LDREQ_Clear()), alongside the
SAVED columns themselves (G_No[0..3], G_Timer, Exit_No, Exit_Timer). The
task-#66 driver next door drives that trace by varying AFS latency. This one
drives the SAME trace by varying ROLLBACK INJECTION instead: two runs of one
binary, identical scripted inputs, pinned RNG, ASLR off, differing only in
whether --rbd-* rollback cycles are on. Column-diff the two CSVs.

READING THE RESULT. A divergent loader column with every SAVED column
identical means the state escaped the save set but was not observed on that
trajectory. It is NOT by itself an exoneration — it is a coverage statement
about the frames the scenario actually visits — so pair it with the ordering
argument that says the reader cannot be dispatched inside the window. For
plt_req that argument is written out in tools/rollback-determinism/
allowlist.txt and in the disposition block at the top of
src/sf33rd/Source/Game/io/gd3rd.c.

This script only INVOKES the game binary with existing flags and reuses the
rbd driver's ASLR-disabling spawn helper. It modifies nothing.

Usage:
    tools/ldreq-timing/rollback_plt_req_probe.py [outdir]
Env:
    PLT_FRAMES   trace length, default 1500
"""

import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
sys.path.insert(0, os.path.join(REPO_ROOT, "tools", "rollback-determinism"))
import check_rollback_determinism as rbd  # noqa: E402

BIN = os.path.join(REPO_ROOT, "build", "host", "3S-ARM.app", "Contents", "MacOS", "3S-ARM")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO_ROOT, "build", "plt-req-probe")
FRAMES = int(os.environ.get("PLT_FRAMES", "1500"))

# The subset of trace columns that is inside the rollback save set, and
# therefore inside the desync checksum: Exit_No/Exit_Timer (game_state.c:
# 456/1196, 607/1347) and G_No/G_Timer (:453, :540). Divergence here IS the
# desync; divergence anywhere else is loader surface.
SAVED = ["G_No0", "G_No1", "G_No2", "G_No3", "G_Timer", "Exit_No", "Exit_Timer"]

PRESETS = {
    "ryu-ken-basic-exchange": ["--test-scene-preset", "basic-exchange"],
    "makoto-sa3-super": ["--test-scene-preset", "yun-sa3-repeat",
                         "--test-p1-character", "16", "--test-p1-super-art", "2"],
}


def run(tag, preset, rollback, barrier, select_depth=8, select_period=8):
    os.makedirs(OUT, exist_ok=True)
    csv = os.path.join(OUT, f"{tag}.csv")
    log = os.path.join(OUT, f"{tag}.log")
    # --rbd-capture is what arms RollbackDeterminism_PreFrame(); the .rbd
    # stream itself is not used here, only the rollback injection it enables.
    # --rbd-frames is set above --ldreq-trace-frames so the trace closes and
    # exits cleanly first.
    args = [BIN, "--test-enable", "--test-pin-rng",
            "--rbd-capture", os.path.join(OUT, f"{tag}.rbd"),
            "--rbd-symmap", os.path.join(OUT, "symmap.txt"),
            "--rbd-frames", str(FRAMES + 500)]
    if rollback:
        args += ["--rbd-rollback-period", "1", "--rbd-rollback-depth", "3",
                 "--rbd-select-rollback-period", str(select_period),
                 "--rbd-select-rollback-depth", str(select_depth)]
    if barrier:
        args += ["--ldreq-barrier-force"]
    args += ["--ldreq-trace", csv, "--ldreq-trace-frames", str(FRAMES)]
    args += PRESETS[preset]
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"
    with open(log, "w") as lf:
        pid = rbd.spawn_no_aslr_darwin(args, env, lf.fileno(), REPO_ROOT)
        rc = rbd.wait_with_timeout(pid, 1200, log)
    return csv, rc


def load(csv):
    rows, hdr = [], None
    with open(csv) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if hdr is None:
                hdr = line.split(",")
                continue
            cells = line.split(",")
            if len(cells) == len(hdr):
                rows.append(cells)
    return hdr, rows


def compare(tag_a, csv_a, tag_b, csv_b):
    """Return (saved_divergent_columns, loader_divergent_columns)."""
    ha, ra = load(csv_a)
    hb, rb = load(csv_b)
    assert ha == hb, (ha, hb)
    n = min(len(ra), len(rb))
    print(f"\n=== {tag_a}  vs  {tag_b}   ({n} frames compared; "
          f"{len(ra)} / {len(rb)} rows) ===")
    saved_div, loader_div = [], []
    for ci, col in enumerate(ha):
        if col == "frame":
            continue
        frames = [i for i in range(n) if ra[i][ci] != rb[i][ci]]
        if not frames:
            continue
        (saved_div if col in SAVED else loader_div).append(col)
        ex = frames[0]
        mark = " [SAVED]" if col in SAVED else ""
        print(f"  {col:<16}{mark:<9} divergent_frames={len(frames):<5} "
              f"first={ra[ex][0]} last={ra[frames[-1]][0]} "
              f"a={ra[ex][ci]} b={rb[ex][ci]}")
    if not saved_div and not loader_div:
        print("  IDENTICAL on every column")
    # The frame the only in-select reader of plt_req is first dispatched on.
    ei = ha.index("Exit_No")
    for run_rows, name in ((ra, tag_a), (rb, tag_b)):
        hit = [i for i in range(len(run_rows)) if run_rows[i][ei] == "5"]
        print(f"  {name}: Exit_No==5 (Exit_6th dispatched) at {hit[:4]}")
    return saved_div, loader_div


def main():
    os.makedirs(OUT, exist_ok=True)
    rbd.build_symbol_map_macho(BIN, os.path.join(OUT, "symmap.txt"))
    all_saved = []
    for preset in PRESETS:
        for barrier in (False, True):
            b = "bar1" if barrier else "bar0"
            base = f"{preset}.{b}.norb"
            ca, rca = run(base, preset, False, barrier)
            for depth, period, suffix in ((8, 8, "rb8"), (2, 8, "rb2"), (8, 1, "rb8p1")):
                tag = f"{preset}.{b}.{suffix}"
                cb, rcb = run(tag, preset, True, barrier, depth, period)
                print(f"\n##### preset={preset} barrier_force={int(barrier)} "
                      f"select_depth={depth} select_period={period} "
                      f"exit: norb={rca} rollback={rcb}")
                if rca != 0 or rcb != 0:
                    print("  ERROR: a run exited nonzero; result is not usable")
                    return 2
                saved, _ = compare(base, ca, tag, cb)
                all_saved += [(tag, c) for c in saved]
    print(f"\nPLT-REQ PROBE SUMMARY: frames={FRAMES} "
          f"saved_column_divergences={len(all_saved)} "
          f"columns={','.join(sorted({c for _, c in all_saved})) or 'none'} "
          f"verdict={'FAIL' if all_saved else 'PASS'}")
    return 1 if all_saved else 0


if __name__ == "__main__":
    sys.exit(main())
