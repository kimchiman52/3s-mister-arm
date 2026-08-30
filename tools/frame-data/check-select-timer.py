#!/usr/bin/env python3
"""Task #108 -- prove the character-select timer actually TICKS in a harness run.

Why this exists
---------------
The 94-corpus frame-data suite drives ``--test-scene-preset training-frame-data``,
which boots training mode. ``Present_Mode`` is then
``PRESENT_MODE_NORMAL_TRAINING`` (4) or ``PRESENT_MODE_PARRY_TRAINING`` (5) for
the whole run, and the character-select countdown runner opens with

    if (Present_Mode == 4 || Present_Mode == 5) { return; }

(``src/sf33rd/Source/Game/effect/effa5.c:49-51``). So every corpus run ENTERS
the select-timer runner and returns before doing anything. Measured on this
tree: 280 entries, 280 early returns, zero ``Select_Timer`` decrements. Select
coverage that never advances the select timer is a claim with nothing behind
it -- and the early return sits ABOVE the arcade/PS2 split, so it hides select
behaviour on BOTH engines equally.

The early return is correct game behaviour (a training-mode select screen has
no countdown) and is not touched. What this script does instead is run the
harness in a configuration where the timer CAN tick -- a non-training scene
preset, so ``Present_Mode`` is ``PRESENT_MODE_LOCAL`` (1), plus
``--test-select-dwell-frames`` so the run actually inhabits the select screen
for longer than ``UNIT_OF_TIMER_MAX`` (50, ``src/constants.h:6``), the period
of one decrement -- and then checks the observation instead of assuming it.

It runs BOTH balance paths (``--test-balance ps2`` and ``--test-balance
arcade``), because "select-screen coverage" that only holds on the engine that
does not ship is the same failure one level down. The arcade leg is skipped,
loudly and as a reported SKIP rather than a silent pass, when no verified CPS3
romset is reachable.

Usage
-----
    python3 tools/frame-data/check-select-timer.py [--cps3-zip PATH]
                                                   [--dwell-frames N]
                                                   [--timeout SECONDS]
                                                   [--baseline]

``--baseline`` additionally runs the training-frame-data preset and reports the
zero-tick baseline this exists to beat (that leg needs a compiled corpus, so it
compiles ``corpus-smoke.yaml`` into a temp dir first).

Requires a #if DEBUG build (build/host, which run.sh/run-suite.sh configure as
CMAKE_BUILD_TYPE=Debug) or -DENABLE_DEBUG_HOOKS=ON: `--test-select-dwell-frames`
lives in src/test/test_runner.c, which is wrapped in `#if defined(DEBUG)` end
to end. The probe itself (frame_trace.c) is NOT so gated, and neither is
`--test-balance`.

Exit 0 iff every leg that ran observed at least one tick; 1 otherwise.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
BIN_PATH = REPO_ROOT / "build" / "host" / "3S-ARM.app" / "Contents" / "MacOS" / "3S-ARM"

# Non-training preset: PHASE_MENU's training-mode branch (test_runner.c) is not
# taken, so Mode_Type stays arcade/versus and Present_Mode is
# PRESENT_MODE_LOCAL (1, src/sf33rd/Source/Game/engine/workuser.h:20) on the
# select screen -- the one thing effa5.c's early return keys on.
NON_TRAINING_PRESET = "basic-exchange"

# UNIT_OF_TIMER_MAX is 50 (src/constants.h:6), so a dwell below ~50 frames
# cannot produce a decrement no matter what else is right. 400 gives ~8.
DEFAULT_DWELL_FRAMES = 400

ROW_RE = re.compile(
    r"^SELPROBE call=(?P<call>\d+) pm=(?P<pm>\d+) rno=(?P<rno>\d+) "
    r"uot=(?P<uot>\d+) st=0x(?P<st>[0-9A-Fa-f]+) early=(?P<early>\d+)$"
)


class ProbeResult:
    def __init__(self, rows):
        self.entries = len(rows)
        self.early_returns = sum(r["early"] for r in rows)
        self.present_modes = sorted({r["pm"] for r in rows})
        self.ticks = sum(1 for a, b in zip(rows, rows[1:]) if a["st"] != b["st"])
        self.first_st = rows[0]["st"] if rows else None
        self.last_st = rows[-1]["st"] if rows else None

    def summary(self):
        return (
            f"entries={self.entries} early_returns={self.early_returns} "
            f"Present_Mode={self.present_modes} select_timer_ticks={self.ticks} "
            f"Select_Timer=0x{self.first_st:02X}->0x{self.last_st:02X}"
            if self.entries
            else "entries=0 (the select-timer runner was never even entered)"
        )


def parse_probe(path):
    rows = []
    with open(path, "r") as fh:
        for line in fh:
            m = ROW_RE.match(line.strip())
            if not m:
                continue
            rows.append(
                {
                    "call": int(m.group("call")),
                    "pm": int(m.group("pm")),
                    "rno": int(m.group("rno")),
                    "uot": int(m.group("uot")),
                    "st": int(m.group("st"), 16),
                    "early": int(m.group("early")),
                }
            )
    return rows


def run_leg(argv, env, probe_path, timeout):
    """Run one harness invocation and parse its probe file.

    The non-training presets never reach a scripted exit, so the run is
    EXPECTED to be killed by the timeout; the probe file is flushed per row
    (frame_trace.c) and is complete regardless. A nonzero exit is therefore not
    itself a failure here -- an exit 6 is, because that is arcade-balance
    unavailability and must not be read as a pass.
    """
    if probe_path.exists():
        probe_path.unlink()
    try:
        proc = subprocess.run(
            argv,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout + 30,
        )
        rc = proc.returncode
        out = proc.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired as e:
        # No GNU timeout/gtimeout on this host, so subprocess's own deadline
        # did the killing. Not a failure: the probe writes and flushes per
        # row, so the file on disk is complete regardless of how the process
        # ended. Fall through and judge the observation, not the exit code.
        rc = None
        out = (e.output or b"").decode("utf-8", "replace")
    if rc == 6:
        return None, out, "arcade balance unavailable (exit 6)"
    if not probe_path.exists():
        return None, out, "probe file was never written (is FD_SELECT_PROBE plumbed?)"
    return ProbeResult(parse_probe(probe_path)), out, None


def timeout_wrapper(seconds):
    for candidate in ("timeout", "gtimeout"):
        if shutil.which(candidate):
            return [candidate, str(seconds)]
    # No GNU timeout: rely on subprocess's own timeout, which raises. Callers
    # treat that as a hard failure rather than silently passing.
    return []


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cps3-zip", default=os.environ.get("FDH_CPS3_ZIP"),
                    help="dev-only romset override for the arcade leg ($THIRDSARM_CPS3_ZIP)")
    ap.add_argument("--dwell-frames", type=int, default=DEFAULT_DWELL_FRAMES)
    ap.add_argument("--timeout", type=int, default=90, help="wall-clock cap per leg, seconds")
    ap.add_argument("--baseline", action="store_true",
                    help="also run the training-frame-data preset and report its zero-tick baseline")
    args = ap.parse_args(argv)

    if not BIN_PATH.exists():
        print(f"error: {BIN_PATH} not found -- build build/host first", file=sys.stderr)
        return 1

    tmpdir = Path(tempfile.mkdtemp(prefix="t108-select-"))
    probe_path = tmpdir / "select-probe.txt"
    failures = []
    skips = []

    base_env = dict(os.environ)
    base_env["SDL_VIDEODRIVER"] = "dummy"
    base_env["SDL_AUDIODRIVER"] = "dummy"
    base_env["FD_SELECT_PROBE"] = str(probe_path)
    base_env.pop("THIRDSARM_CPS3_ZIP", None)

    wrapper = timeout_wrapper(args.timeout)

    for balance in ("ps2", "arcade"):
        env = dict(base_env)
        if balance == "arcade":
            if not args.cps3_zip or not Path(args.cps3_zip).is_file():
                skips.append("arcade: no romset (pass --cps3-zip / set FDH_CPS3_ZIP)")
                print(f"SKIP  arcade  -- {skips[-1]}")
                continue
            env["THIRDSARM_CPS3_ZIP"] = args.cps3_zip

        argv_leg = wrapper + [
            str(BIN_PATH),
            "--test-enable",
            "--test-balance", balance,
            "--test-scene-preset", NON_TRAINING_PRESET,
            "--test-select-dwell-frames", str(args.dwell_frames),
            "--test-pin-rng",
        ]
        result, out, err = run_leg(argv_leg, env, probe_path, args.timeout)
        if err is not None:
            failures.append(f"{balance}: {err}")
            print(f"FAIL  {balance:7s} -- {err}")
            continue
        ok = result.ticks > 0 and result.early_returns == 0
        print(f"{'PASS' if ok else 'FAIL'}  {balance:7s} -- {result.summary()}")
        if not ok:
            failures.append(f"{balance}: {result.summary()}")

    if args.baseline:
        # The training preset needs a compiled corpus to have any input script
        # at all; corpus-smoke is the cheapest one that exists.
        rundir = tmpdir / "smoke"
        rundir.mkdir()
        subprocess.run(
            [sys.executable, str(SCRIPT_DIR / "compile_corpus.py"),
             str(SCRIPT_DIR / "corpus-smoke.yaml"), str(rundir)],
            check=True, stdout=subprocess.DEVNULL,
        )
        meta = json.loads((rundir / "meta.json").read_text())
        env = dict(base_env)
        env["FRAME_TRACE_PATH"] = str(rundir / "trace.log")
        argv_leg = wrapper + [
            str(BIN_PATH),
            "--test-enable",
            "--test-balance", "ps2",
            "--test-scene-preset", "training-frame-data",
            "--test-input-script", str(rundir / "script.fdi"),
            "--test-pin-rng",
            "--test-p1-character", str(meta["p1_character"]),
        ]
        result, out, err = run_leg(argv_leg, env, probe_path, args.timeout)
        if err is not None:
            print(f"BASELINE  unavailable -- {err}")
        else:
            print(f"BASELINE  training-frame-data -- {result.summary()}")
            print("          (this is the zero-tick state the two legs above exist to beat)")

    if failures:
        print(f"\nSELECT-TIMER: FAIL ({len(failures)} leg(s)): " + "; ".join(failures))
        return 1
    if skips and len(skips) == 2:
        print("\nSELECT-TIMER: no leg ran -- refusing to report a pass")
        return 1
    print(f"\nSELECT-TIMER: OK ({2 - len(skips)} leg(s) observed a tick"
          + (f", {len(skips)} skipped)" if skips else ")"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
