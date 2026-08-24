#!/usr/bin/env python3
"""Rollback-determinism harness driver.

Empirically verifies that the rollback save/load whitelist in
src/netplay/game_state.c captures every piece of mutable state the
simulation depends on. See docs/rollback-determinism-harness.md for the
full design and how to read the output.

Per scenario it runs the game binary three times with identical
deterministic inputs (test runner scene preset + pinned RNG):

  A1, A2  baseline: straight-line simulation, no rollbacks
  B       rollback: every --rollback-period frames the game additionally
          performs save -> speculative-resimulate(depth) -> load through
          the PRODUCTION save_state()/load_state_from_event() path

Each run hashes every writable data/bss symbol once per frame (symbol map
generated here from nm on the exact binary) into a stream file. Then:

  noise      = symbols whose hashes differ between A1 and A2 (process-level
               nondeterminism: ASLR'd stored pointers, audio-thread timing,
               allocator addresses). Excluded from the verdict, listed for
               transparency.
  divergent  = symbols whose hashes differ between A1 and B, minus noise,
               minus the explicit allowlist (tools/rollback-determinism/
               allowlist.txt — each entry carries a reason).

Any remaining divergent symbol is state that escaped the rollback save
set: the report gives symbol name, address, size, first/last divergent
frame, divergent-frame count, and whether the symbol is itself in the
GS_SAVE whitelist (a SAVED symbol diverging means some escapee FEEDS BACK
into simulation state — the genuine desync class).

Exit codes: 0 = no unexplained divergence; 1 = divergence found;
2 = harness/plumbing failure. The last stdout line is always the
machine-greppable "RBD SUMMARY: ..." verdict line.
"""

import argparse
import fnmatch
import json
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))

STREAM_MAGIC = 0x31444252  # "RBD1"
FOOTER_MAGIC = 0x46444252  # "RBDF"

# Scenario = (name, extra game args). Every scenario boots through title /
# menu / character select (rollback cycles active from character select
# onward, mirroring netplay's session window) and then plays a scripted
# VS-mode fight. Character indices are the 3SX (non-CPS3) enum in
# src/constants.h.
FAST_SCENARIOS = [
    # Generic two-character exchange: attacks, jumps, specials on both
    # sides. Covers character select + the bread-and-butter battle loop.
    ("ryu-ken-basic-exchange", ["--test-scene-preset", "basic-exchange"]),
    # Positive control: Makoto (3SX index 16) SA3. Her super activation
    # spawns effect L8 (effl8.c), whose spmv_ng_save[] file-static is a
    # KNOWN rollback escapee being fixed separately — the harness must
    # flag it, proving end-to-end detection. The *-repeat preset scripts
    # QCFx2+P supers at fixed frames; character/SA overrides retarget it.
    ("makoto-sa3-super", ["--test-scene-preset", "yun-sa3-repeat",
                          "--test-p1-character", "16", "--test-p1-super-art", "2"]),
]

# Thorough: every selectable character as P1 (1..19; Gill=0 is not
# selectable through the character-select flow the test runner drives)
# against the preset's Ryu, with the pressure variant (constant contact:
# attacks, blocks, throws, jump-ins) plus two scripted super attempts.
THOROUGH_SCENARIOS = FAST_SCENARIOS + [
    (f"char{c:02d}-pressure-super",
     ["--test-scene-preset", "yun-sa3-repeat-pressure",
      "--test-p1-character", str(c), "--test-p1-super-art", "2"])
    for c in range(1, 20)
]


def log(msg):
    print(f"[rbd-driver] {msg}", file=sys.stderr, flush=True)


class ScenarioError(Exception):
    """A single scenario's runs failed (crash, timeout, truncated stream,
    zero rollback cycles, never in-game). Contained per scenario so a
    crash-class finding in one character's sweep doesn't hide the results
    of the other 20 — the scenario is reported as ERROR and the driver
    still exits 2. A scenario ERROR in a rollback run usually IS a
    finding (a crash-class rollback bug); see the per-run .log next to
    the streams."""


def fail(msg):
    log(f"ERROR: {msg}")
    print("RBD SUMMARY: verdict=ERROR")
    sys.exit(2)


# --- symbol map ------------------------------------------------------------

def macho_writable_sections(binary):
    """Parse `otool -l` for the writable __DATA segment's section bounds."""
    out = subprocess.run(["otool", "-l", binary], check=True,
                         capture_output=True, text=True).stdout
    sections = []
    cur_sect = None
    cur_seg = None
    addr = None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("sectname "):
            cur_sect = line.split()[1]
            cur_seg = None
            addr = None
        elif line.startswith("segname "):
            cur_seg = line.split()[1]
        elif line.startswith("addr "):
            addr = int(line.split()[1], 16)
        elif line.startswith("size ") and cur_sect and cur_seg == "__DATA" and addr is not None:
            size = int(line.split()[1], 16)
            sections.append((cur_sect, addr, size))
            cur_sect = None
    if not sections:
        fail(f"no writable __DATA sections found in {binary}")
    return sections


NM_MACHO_RE = re.compile(
    r"^([0-9a-f]+) \(__DATA,(__data|__bss|__common)\) "
    r"(?:(?:non-external|external|weak|private|was) )*(.+)$")


def build_symbol_map_macho(binary, map_path):
    sections = {name: (addr, size) for name, addr, size in macho_writable_sections(binary)}
    out = subprocess.run(["nm", "-nm", binary], check=True,
                         capture_output=True, text=True).stdout
    per_section = {name: [] for name in sections}
    for line in out.splitlines():
        m = NM_MACHO_RE.match(line)
        if not m:
            continue
        addr, sect, name = int(m.group(1), 16), m.group(2), m.group(3)
        if name.startswith("_"):
            name = name[1:]  # strip C mangling underscore
        # nm output has spaces only in odd local names; normalize them away
        name = name.replace(" ", ".")
        per_section[sect].append((addr, name))

    entries = []
    for sect, (sect_addr, sect_size) in sections.items():
        syms = sorted(per_section[sect])
        sect_end = sect_addr + sect_size
        if syms and syms[0][0] > sect_addr:
            entries.append((sect_addr, syms[0][0] - sect_addr, f"{sect}.headgap"))
        for i, (addr, name) in enumerate(syms):
            end = syms[i + 1][0] if i + 1 < len(syms) else sect_end
            size = end - addr
            if size <= 0:
                continue
            entries.append((addr, size, name))

    entries.sort()
    write_map(entries, map_path)
    return entries


def build_symbol_map_elf(binary, map_path):
    out = subprocess.run(["nm", "-S", "--defined-only", binary], check=True,
                         capture_output=True, text=True).stdout
    entries = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 4:
            continue
        addr_s, size_s, typ, name = parts
        if typ not in ("d", "D", "b", "B"):
            continue
        entries.append((int(addr_s, 16), int(size_s, 16), name))
    entries.sort()
    write_map(entries, map_path)
    return entries


def write_map(entries, map_path):
    with open(map_path, "w") as f:
        for addr, size, name in entries:
            if name.startswith("rbd_"):
                continue  # harness bookkeeping — the game skips these too
            f.write(f"{addr:x} {size:x} {name}\n")


def load_map(map_path):
    entries = []
    with open(map_path) as f:
        for line in f:
            a, s, n = line.split()
            entries.append((int(a, 16), int(s, 16), n))
    return entries


# --- stream files ----------------------------------------------------------

class Stream:
    __slots__ = ("sym_count", "rows", "frames", "cycles", "ingame_first")

    def __init__(self, sym_count, rows, frames, cycles, ingame_first):
        self.sym_count = sym_count
        self.rows = rows          # list[bytes], each sym_count*4 bytes
        self.frames = frames
        self.cycles = cycles
        self.ingame_first = ingame_first


def read_stream(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 8:
        raise ScenarioError(f"stream {path} truncated (no header)")
    magic, sym_count = struct.unpack_from("<II", data, 0)
    if magic != STREAM_MAGIC:
        raise ScenarioError(f"stream {path} has bad magic {magic:#x}")
    row_bytes = 4 + sym_count * 4
    off = 8
    rows = []
    while off + row_bytes <= len(data):
        # Peek: is this the footer instead of a row?
        (maybe_magic,) = struct.unpack_from("<I", data, off)
        if maybe_magic == FOOTER_MAGIC and off + 20 == len(data):
            break
        frame_idx = maybe_magic
        if frame_idx != len(rows):
            raise ScenarioError(f"stream {path}: frame index {frame_idx} at row {len(rows)} — corrupt stream")
        rows.append(data[off + 4:off + row_bytes])
        off += row_bytes
    if off + 20 != len(data):
        raise ScenarioError(f"stream {path}: missing/short footer — run died before completing "
             f"(rows={len(rows)}, trailing={len(data) - off} bytes)")
    magic, frames, cycles, ingame_first, completed = struct.unpack_from("<IIIII", data, off)
    if magic != FOOTER_MAGIC or completed != 1:
        raise ScenarioError(f"stream {path}: bad footer (magic={magic:#x} completed={completed})")
    if frames != len(rows):
        raise ScenarioError(f"stream {path}: footer frames={frames} but rows={len(rows)}")
    return Stream(sym_count, rows, frames, cycles, ingame_first)


# --- game runs -------------------------------------------------------------

def spawn_no_aslr_darwin(args, env, logf_fd, cwd):
    """posix_spawn with _POSIX_SPAWN_DISABLE_ASLR (0x0100, the flag lldb
    and Xcode use). Without this every game process gets its own image
    slide, so every symbol whose CONTENT is a pointer (static pointer
    tables, task[].func_adrs, plw's cached table pointers, ...) differs
    between two otherwise identical runs, flooding the noise set and
    masking exactly the saved-state feedback signal this harness is for.
    Returns the child pid."""
    import ctypes
    libc = ctypes.CDLL(None, use_errno=True)

    attr = ctypes.c_void_p()
    if libc.posix_spawnattr_init(ctypes.byref(attr)) != 0:
        fail("posix_spawnattr_init failed")
    POSIX_SPAWN_DISABLE_ASLR = 0x0100
    if libc.posix_spawnattr_setflags(ctypes.byref(attr),
                                     ctypes.c_short(POSIX_SPAWN_DISABLE_ASLR)) != 0:
        fail("posix_spawnattr_setflags failed")

    fa = ctypes.c_void_p()
    if libc.posix_spawn_file_actions_init(ctypes.byref(fa)) != 0:
        fail("posix_spawn_file_actions_init failed")
    libc.posix_spawn_file_actions_adddup2(ctypes.byref(fa), logf_fd, 1)
    libc.posix_spawn_file_actions_adddup2(ctypes.byref(fa), logf_fd, 2)

    argv = (ctypes.c_char_p * (len(args) + 1))(
        *[a.encode() for a in args], None)
    envp = (ctypes.c_char_p * (len(env) + 1))(
        *[f"{k}={v}".encode() for k, v in env.items()], None)

    pid = ctypes.c_int()
    prev_cwd = os.getcwd()
    os.chdir(cwd)
    try:
        rc = libc.posix_spawn(ctypes.byref(pid), args[0].encode(),
                              ctypes.byref(fa), ctypes.byref(attr), argv, envp)
    finally:
        os.chdir(prev_cwd)
        libc.posix_spawn_file_actions_destroy(ctypes.byref(fa))
        libc.posix_spawnattr_destroy(ctypes.byref(attr))
    if rc != 0:
        fail(f"posix_spawn failed with {rc}")
    return pid.value


def wait_with_timeout(pid, timeout, log_path):
    import signal as _signal
    import time as _time
    deadline = _time.monotonic() + timeout
    while True:
        done, status = os.waitpid(pid, os.WNOHANG)
        if done == pid:
            if os.WIFSIGNALED(status):
                return -os.WTERMSIG(status)
            return os.WEXITSTATUS(status)
        if _time.monotonic() > deadline:
            os.kill(pid, _signal.SIGKILL)
            os.waitpid(pid, 0)
            raise ScenarioError(f"game timed out after {timeout}s (log: {log_path})")
        _time.sleep(0.2)


def run_game(binary, base_args, extra_args, out_path, map_path, frames,
             period, depth, timeout, log_path):
    args = [binary, "--test-enable", "--test-pin-rng",
            "--rbd-capture", out_path, "--rbd-symmap", map_path,
            "--rbd-frames", str(frames)]
    if period > 0:
        args += ["--rbd-rollback-period", str(period),
                 "--rbd-rollback-depth", str(depth)]
    args += base_args + extra_args
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"
    with open(log_path, "w") as logf:
        if platform.system() == "Darwin":
            pid = spawn_no_aslr_darwin(args, env, logf.fileno(), REPO_ROOT)
            returncode = wait_with_timeout(pid, timeout, log_path)
        else:
            # Linux: setarch -R disables ASLR for the child.
            args = ["setarch", "-R"] + args
            try:
                proc = subprocess.run(args, env=env, stdout=logf,
                                      stderr=subprocess.STDOUT,
                                      timeout=timeout, cwd=REPO_ROOT)
            except subprocess.TimeoutExpired:
                raise ScenarioError(f"game timed out after {timeout}s (log: {log_path})")
            returncode = proc.returncode
    if returncode != 0:
        raise ScenarioError(f"game exited with code {returncode} (log: {log_path})")
    return read_stream(out_path)


# --- comparison ------------------------------------------------------------

def diff_streams(ref, other):
    """Returns {sym_index: (first_frame, last_frame, count)}."""
    if ref.sym_count != other.sym_count or len(ref.rows) != len(other.rows):
        raise ScenarioError(f"stream shape mismatch: {ref.sym_count}x{len(ref.rows)} vs "
             f"{other.sym_count}x{len(other.rows)}")
    result = {}
    n = ref.sym_count
    for frame, (ra, rb) in enumerate(zip(ref.rows, other.rows)):
        if ra == rb:
            continue
        for i in range(n):
            o = i * 4
            if ra[o:o + 4] != rb[o:o + 4]:
                if i in result:
                    first, _, count = result[i]
                    result[i] = (first, frame, count + 1)
                else:
                    result[i] = (frame, frame, 1)
    return result


def load_allowlist(path):
    """Lines: `pattern  # reason`. fnmatch patterns against symbol names."""
    entries = []
    if not os.path.exists(path):
        return entries
    with open(path) as f:
        for line_no, line in enumerate(f, 1):
            body, _, comment = line.partition("#")
            body = body.strip()
            if not body:
                continue
            entries.append((body, comment.strip() or "(no reason given)"))
    return entries


def allowlist_match(name, allowlist):
    for pattern, reason in allowlist:
        if fnmatch.fnmatchcase(name, pattern):
            return reason
    return None


def load_gs_save_names():
    """Symbol names covered by GS_SAVE in game_state.c (the whitelist under
    test), used to tag feedback divergence."""
    path = os.path.join(REPO_ROOT, "src", "netplay", "game_state.c")
    names = set()
    with open(path) as f:
        for m in re.finditer(r"GS_SAVE\(([A-Za-z_][A-Za-z_0-9]*)\)", f.read()):
            names.add(m.group(1))
    # EffectState globals are saved through gather_state, not GS_SAVE.
    names.update({"frw", "frwque", "frwctr", "frwctr_min",
                  "head_ix", "tail_ix", "exec_tm"})
    return names


# --- per-scenario pipeline --------------------------------------------------

def run_scenario(name, extra, args, outdir, map_path, frames, entries,
                 allowlist, gs_saved):
    """Run A1/A2/B for one scenario, diff, classify, print, and return the
    scenario dict for report.json. Raises ScenarioError on any run/stream
    failure (contained by the caller)."""
    log(f"scenario {name}: baseline A1")
    runs = {}
    for run_name, period in (("A1", 0), ("A2", 0), ("B", args.rollback_period)):
        if run_name != "A1":
            log(f"scenario {name}: {'baseline ' + run_name if period == 0 else 'rollback B'}")
        out = os.path.join(outdir, f"{name}.{run_name}.rbd")
        runlog = os.path.join(outdir, f"{name}.{run_name}.log")
        runs[run_name] = run_game(args.binary, [], extra, out, map_path,
                                  frames, period, args.rollback_depth,
                                  args.timeout, runlog)

    b = runs["B"]
    if b.cycles == 0:
        raise ScenarioError(f"rollback run executed ZERO rollback cycles — "
                            f"harness is not exercising the save/load path")
    if b.ingame_first == 0xFFFFFFFF:
        raise ScenarioError(f"run never reached in-game (G_No[1]==2) "
                            f"within {frames} frames")

    noise = diff_streams(runs["A1"], runs["A2"])
    div = diff_streams(runs["A1"], b)

    # A GS_SAVE-covered symbol in the noise set means the baseline
    # itself is nondeterministic in gameplay state — the feedback
    # detector is blind there. Warn loudly; that's a harness-health
    # signal, not a divergence verdict.
    noisy_saved = sorted(entries[i][2] for i in noise
                         if entries[i][2] in gs_saved)
    if noisy_saved:
        log(f"WARNING scenario {name}: GS_SAVE-covered symbols are "
            f"baseline-nondeterministic (feedback detection blind for "
            f"them): {', '.join(noisy_saved)}")

    rows = []
    n_noise = n_allow = n_real = n_feedback = 0
    for idx, (first, last, count) in sorted(div.items(), key=lambda kv: kv[1][0]):
        addr, size, sym = entries[idx]
        if idx in noise:
            n_noise += 1
            verdict = "NOISE"
            reason = "differs between the two identical baseline runs"
        else:
            reason = allowlist_match(sym, allowlist)
            if reason is not None:
                n_allow += 1
                verdict = "ALLOWED"
            else:
                n_real += 1
                verdict = "DIVERGENT"
                if sym in gs_saved:
                    verdict = "DIVERGENT+FEEDBACK"
                    n_feedback += 1
        rows.append({"symbol": sym, "addr": f"{addr:#x}", "size": size,
                     "first_frame": first, "last_frame": last,
                     "divergent_frames": count, "verdict": verdict,
                     "saved": sym in gs_saved, "reason": reason})

    # Noise-only symbols (nondeterministic even without rollback) that
    # did NOT show in the A1-vs-B diff are still worth listing.
    noise_names = sorted(entries[i][2] for i in noise)

    print(f"\n=== scenario {name} (in-game from frame {b.ingame_first}, "
          f"{b.cycles} rollback cycles) ===")
    if not rows:
        print("  no divergence at all")
    for r in rows:
        tag = f" [{r['reason']}]" if r["verdict"] in ("ALLOWED",) else ""
        print(f"  {r['verdict']:<18} {r['symbol']:<40} addr={r['addr']} "
              f"size={r['size']} first={r['first_frame']} last={r['last_frame']} "
              f"frames={r['divergent_frames']}{tag}")
    if noise_names:
        print(f"  (baseline noise, excluded: {', '.join(noise_names)})")

    return {"name": name, "args": extra, "frames": frames,
            "rollback_cycles": b.cycles, "ingame_first_frame": b.ingame_first,
            "divergent": n_real, "feedback": n_feedback,
            "allowlisted": n_allow, "noise": len(noise),
            "noise_symbols": noise_names, "rows": rows}


# --- main ------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default=os.path.join(
        REPO_ROOT, "build", "host", "3S-ARM.app", "Contents", "MacOS", "3S-ARM"))
    ap.add_argument("--mode", choices=["fast", "thorough"], default="fast")
    ap.add_argument("--frames", type=int, default=None,
                    help="frames per run (default: 1500 fast, 2400 thorough)")
    ap.add_argument("--rollback-period", type=int, default=1)
    ap.add_argument("--rollback-depth", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=900, help="per-run timeout (s)")
    ap.add_argument("--outdir", default=None, help="work dir (default: mktemp)")
    ap.add_argument("--allowlist", default=os.path.join(SCRIPT_DIR, "allowlist.txt"))
    ap.add_argument("--keep", action="store_true", help="keep work dir on success")
    ap.add_argument("--scenario", action="append", default=None,
                    help="run only scenarios whose name matches this fnmatch "
                         "pattern (repeatable)")
    args = ap.parse_args(argv)

    if not os.path.exists(args.binary):
        fail(f"binary not found: {args.binary} (build build/host first — "
             f"see tools/rollback-determinism/run.sh)")

    frames = args.frames or (1500 if args.mode == "fast" else 2400)
    scenarios = FAST_SCENARIOS if args.mode == "fast" else THOROUGH_SCENARIOS
    if args.scenario:
        scenarios = [s for s in scenarios
                     if any(fnmatch.fnmatchcase(s[0], pat) for pat in args.scenario)]
        if not scenarios:
            fail("no scenarios match --scenario filter")

    outdir = args.outdir or tempfile.mkdtemp(prefix="rbd-")
    os.makedirs(outdir, exist_ok=True)
    log(f"work dir: {outdir}")

    map_path = os.path.join(outdir, "symmap.txt")
    if platform.system() == "Darwin":
        build_symbol_map_macho(args.binary, map_path)
    else:
        build_symbol_map_elf(args.binary, map_path)
    entries = load_map(map_path)
    total_bytes = sum(s for _, s, _ in entries)
    log(f"symbol map: {len(entries)} writable symbols, {total_bytes / 1e6:.1f} MB covered")

    allowlist = load_allowlist(args.allowlist)
    gs_saved = load_gs_save_names()

    report = {"mode": args.mode, "frames": frames,
              "rollback_period": args.rollback_period,
              "rollback_depth": args.rollback_depth,
              "scenarios": []}
    total_divergent = 0
    total_feedback = 0
    total_noise = 0
    total_allowlisted = 0
    total_errors = 0

    for name, extra in scenarios:
        try:
            scenario_result = run_scenario(name, extra, args, outdir, map_path,
                                           frames, entries, allowlist, gs_saved)
        except ScenarioError as e:
            total_errors += 1
            log(f"SCENARIO ERROR {name}: {e}")
            print(f"\n=== scenario {name} ===")
            print(f"  ERROR              {e}")
            print(f"  (a rollback-run crash/hang here is usually a crash-class "
                  f"rollback bug — see the per-run .log files in {outdir})")
            report["scenarios"].append({"name": name, "args": extra,
                                        "error": str(e)})
            continue

        report["scenarios"].append(scenario_result)
        total_divergent += scenario_result["divergent"]
        total_feedback += scenario_result["feedback"]
        total_noise += scenario_result["noise"]
        total_allowlisted += scenario_result["allowlisted"]

    report_path = os.path.join(outdir, "report.json")
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2)
    log(f"report: {report_path}")

    if total_errors > 0:
        verdict = "ERROR"
    elif total_divergent > 0:
        verdict = "FAIL"
    else:
        verdict = "PASS"
    print(f"\nRBD SUMMARY: mode={args.mode} scenarios={len(scenarios)} "
          f"frames={frames} period={args.rollback_period} depth={args.rollback_depth} "
          f"divergent={total_divergent} feedback={total_feedback} "
          f"allowlisted={total_allowlisted} noise={total_noise} "
          f"errors={total_errors} verdict={verdict}")

    if verdict == "PASS" and not args.keep and args.outdir is None:
        shutil.rmtree(outdir, ignore_errors=True)
    else:
        log(f"work dir kept: {outdir}")

    if verdict == "ERROR":
        return 2
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
