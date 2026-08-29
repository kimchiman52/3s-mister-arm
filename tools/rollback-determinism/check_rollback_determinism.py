#!/usr/bin/env python3
"""Rollback-determinism harness driver.

Empirically verifies that the rollback save/load whitelist in
src/netplay/game_state.c captures every piece of mutable state the
simulation depends on. See docs/rollback-determinism-harness.md for the
full design and how to read the output.

Per scenario it runs the game binary three times with identical
deterministic inputs (test runner scene preset + pinned RNG):

  A1, A2  baseline: straight-line simulation, no rollbacks. A2 additionally
          carries a deliberate address-layout perturbation (NOISE_PAD_ENV)
          so the noise control below is a valid control rather than a coin
          flip -- see that constant, and RBD_PTR_TOKEN in
          src/test/rollback_determinism.c, for the #65 write-up.
  B       rollback: every --rollback-period frames the game additionally
          performs save -> speculative-resimulate(depth) -> load through
          the PRODUCTION save_state()/load_state_from_event() path.
          Character select uses its own independent depth
          (--select-rollback-depth), because production predicts 8 frames
          ahead there and the gate used to probe it at 2.

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

STREAM_MAGIC = 0x32444252  # "RBD2" (bumped when the footer gained ptr_canon)
FOOTER_MAGIC = 0x46444252  # "RBDF"
FOOTER_BYTES = 24  # magic, frames, cycles, ingame_first, ptr_canon, completed

# Environment variable set ONLY on the A2 baseline, to make the A1-vs-A2
# noise control a valid control for stack/argv/env-valued symbols (task #65).
#
# The value is never read by the game — it exists purely for its SIZE. The
# initial process stack sits beneath the argv/envp string block, so
# lengthening the environment provably moves the stack, the argv strings and
# the env strings. Measured on this host through the driver's own spawn path
# (12 runs unpadded vs 12 runs with a 64-byte pad, probe binary): unpadded
# runs took only TWO distinct stack addresses (0x16fdfe0fc / 0x16fdfe10c —
# about one bit of entropy, so a 57% chance that two runs coincide), while
# padded runs took a third value disjoint from both, with ZERO overlap
# between the padded and unpadded sets on any of stack / argv[0] / envp.
#
# Without this, a symbol holding an argv or stack address is excluded as
# NOISE only when the two baselines happen to land on different values — 43%
# of the time. `configuration` (which holds const char* into argv) is the
# documented example, and it only stayed benign because it carries an
# allowlist entry of its own; a symbol without one would surface as a false
# DIVERGENT exactly the way the four port/ pointers in #65 did. With the pad
# the two baselines can never agree on those addresses, so the exclusion is
# structural instead of a coin flip.
#
# This does NOT cover heap addresses (the pad cannot move mmap placements);
# those are handled in the capture itself — see RBD_PTR_TOKEN in
# src/test/rollback_determinism.c.
NOISE_PAD_ENV = "RBD_ADDRESS_LAYOUT_PAD"
NOISE_PAD_BYTES = 64

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
    __slots__ = ("sym_count", "rows", "frames", "cycles", "ingame_first",
                 "ptr_canon")

    def __init__(self, sym_count, rows, frames, cycles, ingame_first, ptr_canon):
        self.sym_count = sym_count
        self.rows = rows          # list[bytes], each sym_count*4 bytes
        self.frames = frames
        self.cycles = cycles
        self.ingame_first = ingame_first
        # How many symbols the capture's whole-pointer canonicalization rule
        # fired on (RBD_PTR_TOKEN in src/test/rollback_determinism.c). Carried
        # so run_scenario can cross-check the three runs against each other —
        # nothing this harness suppresses is allowed to be invisible.
        self.ptr_canon = ptr_canon


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
        if maybe_magic == FOOTER_MAGIC and off + FOOTER_BYTES == len(data):
            break
        frame_idx = maybe_magic
        if frame_idx != len(rows):
            raise ScenarioError(f"stream {path}: frame index {frame_idx} at row {len(rows)} — corrupt stream")
        rows.append(data[off + 4:off + row_bytes])
        off += row_bytes
    if off + FOOTER_BYTES != len(data):
        raise ScenarioError(f"stream {path}: missing/short footer — run died before completing "
             f"(rows={len(rows)}, trailing={len(data) - off} bytes)")
    magic, frames, cycles, ingame_first, ptr_canon, completed = struct.unpack_from(
        "<IIIIII", data, off)
    if magic != FOOTER_MAGIC or completed != 1:
        raise ScenarioError(f"stream {path}: bad footer (magic={magic:#x} completed={completed})")
    if frames != len(rows):
        raise ScenarioError(f"stream {path}: footer frames={frames} but rows={len(rows)}")
    return Stream(sym_count, rows, frames, cycles, ingame_first, ptr_canon)


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
             period, depth, select_period, select_depth, timeout, log_path,
             address_layout_pad=False):
    args = [binary, "--test-enable", "--test-pin-rng",
            "--rbd-capture", out_path, "--rbd-symmap", map_path,
            "--rbd-frames", str(frames)]
    if period > 0:
        # The select-phase knobs are passed EXPLICITLY rather than inherited
        # from the game's compiled-in defaults (task #63). They only matter
        # when rollbacks are enabled at all, so they ride with the in-game
        # pair. Passing them means the depth the gate actually exercises at
        # character select is visible in the per-run log and in the RBD
        # SUMMARY line, and cannot drift silently if a default changes.
        args += ["--rbd-rollback-period", str(period),
                 "--rbd-rollback-depth", str(depth),
                 "--rbd-select-rollback-period", str(select_period),
                 "--rbd-select-rollback-depth", str(select_depth)]
    args += base_args + extra_args
    env = dict(os.environ)
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"
    if address_layout_pad:
        # See NOISE_PAD_ENV. Inert to the game; present only to shift the
        # initial stack / argv / envp addresses so the noise control is a
        # valid control for symbols that store one.
        env[NOISE_PAD_ENV] = "x" * NOISE_PAD_BYTES
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


# Saved globals the `GS_SAVE(...)` regex below structurally cannot see.
#
# HAND_ROLLED_GS_SAVES: four globals are saved with a local `extern` +
# explicit SDL_memcpy block instead of the GS_SAVE macro, because the macro
# takes a bare name and these are externs with no header the save file
# includes (game_state.c, the `chainex_check`/`Color7`/`ca_check_flag`/
# `spmv_ng_save` blocks near the end of GameState_Save). Missing them is not
# cosmetic: they are FOUR OF THE FIVE historic whitelist escapees this
# harness exists to catch, so a regression in any of them would print plain
# DIVERGENT instead of DIVERGENT+FEEDBACK, losing exactly the signal that
# says "saved state drifted, so something is feeding back".
HAND_ROLLED_GS_SAVES = ("chainex_check", "Color7", "ca_check_flag", "spmv_ng_save")

# PARTIAL_GS_SAVES: GameState field -> the global it saves a SLICE of. These
# are presence-checked exactly like HAND_ROLLED_GS_SAVES, but deliberately NOT
# added to the saved-name set: only part of the global is restored, so tagging
# the whole symbol as saved would mislabel a divergence in the unsaved
# remainder as FEEDBACK. The check exists so the slice save can't be dropped
# silently.
PARTIAL_GS_SAVES = {"effl8_colorram": "ColorRAM"}

# EffectState globals are saved through gather_state(), not GS_SAVE.
EFFECT_STATE_SAVES = ("frw", "frwque", "frwctr", "frwctr_min",
                      "head_ix", "tail_ix", "exec_tm")

# Floor for the macro-extracted name count. The set only ever grows in normal
# work (adding a field to the save set), so a DROP means the regex stopped
# matching — e.g. someone renamed/reshaped the GS_SAVE macro — which would
# silently blind the feedback detector rather than fail. Raise this when the
# save set legitimately grows; never lower it to make a run pass.
#
# Kept flush with the actual count (607 unique names as of this line) rather
# than left slack: a floor with slack is a floor you can walk under. Flush also
# gives it a second, stronger meaning — the save set may never SHRINK without a
# deliberate edit here. Growth still passes untouched, so adding fields needs no
# maintenance; only removal does, which is the case that deserves friction.
#
# The floor is still the WEAKER of the two guards, because it only sees the
# aggregate — see the GS_SAVE/GS_LOAD set-equality check in
# load_gs_save_names(), which is derived rather than pinned and catches a single
# dropped line at any save-set size, which no fixed number can.
#
# Deliberately removing a field from the save set (or running the Control B
# mutation test in docs/rollback-determinism-harness.md, which deletes a
# GS_SAVE/GS_LOAD pair on purpose) therefore needs this number lowered to
# match. That is the intended workflow, not a workaround — but only ever in a
# scratch tree or alongside a real save-set change. Never lower it to make an
# otherwise-failing run go green.
#
# Lowered 608 -> 607 on 2026-08-29 by task #109, which is exactly the
# "alongside a real save-set change" case above: GS_SAVE(select_timer_state) /
# GS_LOAD(select_timer_state) were removed together with the GameState member
# and the dead src/sf33rd/Source/Game/select_timer.{c,h} module that declared
# its type. Upstream 33dfd75b (#216) had already moved that countdown into
# effect A5 and deleted the module; our a752e2ca omnibus squash re-added the
# module with ZERO call sites, so the field was saved and loaded as permanent
# zeros on every rollback frame. Exactly one name left the save set, the
# GS_SAVE/GS_LOAD set-equality guard below still holds (both halves were
# removed), and the regex itself is unchanged -- so this is a shrink by
# deliberate edit, not a stale-regex blinding.
MIN_GS_SAVE_MACRO_NAMES = 607


def load_gs_save_names():
    """Symbol names covered by the rollback save set in game_state.c (the
    whitelist under test), used to tag feedback divergence.

    Fails the run (exit 2) rather than returning a quietly-degraded set: an
    under-populated whitelist downgrades DIVERGENT+FEEDBACK findings to plain
    DIVERGENT, which is a silent loss of the harness's sharpest signal.
    """
    path = os.path.join(REPO_ROOT, "src", "netplay", "game_state.c")
    with open(path) as f:
        text = f.read()

    names = set(re.findall(r"GS_SAVE\(([A-Za-z_][A-Za-z_0-9]*)\)", text))
    if len(names) < MIN_GS_SAVE_MACRO_NAMES:
        fail(f"GS_SAVE extraction recognised only {len(names)} names in "
             f"{path} (floor is {MIN_GS_SAVE_MACRO_NAMES}). The save-set "
             f"regex has gone stale — feedback tagging would be wrong. Fix "
             f"the extraction in load_gs_save_names(), do not lower the floor. "
             f"(If you deliberately removed a field from the save set, or are "
             f"running the Control B mutation test, lower the floor to match "
             f"in that tree — see the comment on MIN_GS_SAVE_MACRO_NAMES.)")

    # Derived companion to the floor above. GS_SAVE and GS_LOAD are two halves
    # of one round trip, so their name sets must be IDENTICAL — that invariant
    # needs no pinned number and cannot drift as the save set grows. It closes
    # the floor's blind spot: the floor only sees the aggregate, so dropping a
    # handful of GS_SAVE lines stays above it and passes, while dropping even
    # one shows up here as an asymmetry. A save without its load (or a load
    # without its save) is also a bug in its own right: the state is captured
    # and never restored, or restored from a stale field that nothing writes.
    load_names = set(re.findall(r"GS_LOAD\(([A-Za-z_][A-Za-z_0-9]*)\)", text))
    save_only = sorted(names - load_names)
    load_only = sorted(load_names - names)
    if save_only or load_only:
        detail = []
        if save_only:
            detail.append(f"GS_SAVE without GS_LOAD: {', '.join(save_only)}")
        if load_only:
            detail.append(f"GS_LOAD without GS_SAVE: {', '.join(load_only)}")
        fail(f"GS_SAVE/GS_LOAD name sets disagree in {path} "
             f"({len(names)} save / {len(load_names)} load). "
             f"{'; '.join(detail)}. Every saved field must be loaded and vice "
             f"versa; a one-sided entry means state is captured but never "
             f"restored (or restored from a field nothing writes). Fix the "
             f"save set, do not relax this check.")

    # Every hand-rolled/partial entry must still be findable on BOTH halves of
    # the round trip: `dst->NAME` in GameState_Save and `src->NAME` in
    # GameState_Load. Checking only one half would let a rename that drops the
    # save while leaving the load (or vice versa) pass silently — which is the
    # precise failure mode this guard exists to prevent.
    def both_halves_present(field):
        return (re.search(r"dst->" + re.escape(field) + r"\b", text) and
                re.search(r"src->" + re.escape(field) + r"\b", text))

    missing = [n for n in HAND_ROLLED_GS_SAVES if not both_halves_present(n)]
    if missing:
        fail(f"hand-rolled GS_SAVE/GS_LOAD block(s) not found in {path}: "
             f"{', '.join(missing)}. These are historic whitelist escapees; "
             f"if one was genuinely removed from the save set, update "
             f"HAND_ROLLED_GS_SAVES — otherwise the extraction is stale.")
    names.update(HAND_ROLLED_GS_SAVES)

    partial_missing = [f"{field} (slice of {glob})"
                       for field, glob in PARTIAL_GS_SAVES.items()
                       if not both_halves_present(field)]
    if partial_missing:
        fail(f"partial GS_SAVE/GS_LOAD slice(s) not found in {path}: "
             f"{', '.join(partial_missing)}. If a slice save was genuinely "
             f"removed, update PARTIAL_GS_SAVES — otherwise the extraction is "
             f"stale.")

    # Same guard for the effect-pool members, which live in the EffectState
    # struct rather than in any GS_SAVE line. Scope the search to the struct
    # BODY — matching anywhere in the header would let a prose mention of
    # `frw[]` in a comment satisfy the check after the member itself was
    # renamed away.
    header = os.path.join(REPO_ROOT, "src", "netplay", "game_state.h")
    with open(header) as f:
        header_text = f.read()
    body = re.search(r"typedef\s+struct\s+EffectState\s*\{(.*?)\}\s*EffectState\s*;",
                     header_text, re.S)
    if not body:
        fail(f"could not locate the EffectState struct body in {header}. The "
             f"effect-pool save shape changed shape or moved; update "
             f"load_gs_save_names().")
    es_missing = [n for n in EFFECT_STATE_SAVES
                  if not re.search(r"\b" + re.escape(n) + r"\s*[;\[]", body.group(1))]
    if es_missing:
        fail(f"EffectState member(s) not found in {header}: "
             f"{', '.join(es_missing)}. The effect-pool save shape changed; "
             f"update EFFECT_STATE_SAVES.")
    names.update(EFFECT_STATE_SAVES)

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
        runs[run_name] = run_game(args.binary, [], extra + list(args.game_arg),
                                  out, map_path,
                                  frames, period, args.rollback_depth,
                                  args.select_rollback_period,
                                  args.select_rollback_depth,
                                  args.timeout, runlog,
                                  address_layout_pad=(run_name == "A2"))

    b = runs["B"]
    if b.cycles == 0:
        raise ScenarioError(f"rollback run executed ZERO rollback cycles — "
                            f"harness is not exercising the save/load path")
    if b.ingame_first == 0xFFFFFFFF:
        raise ScenarioError(f"run never reached in-game (G_No[1]==2) "
                            f"within {frames} frames")

    # The capture replaces whole-pointer statics with a fixed token instead of
    # hashing the address (RBD_PTR_TOKEN, src/test/rollback_determinism.c).
    # That is a deliberate suppression, so it is cross-checked rather than
    # trusted: all three runs should fire the rule on the same number of
    # symbols. A mismatch is legitimate in one specific case — a pointer that
    # is NULL in one run and set in another, which the rule deliberately still
    # lets diverge — so this warns with the per-symbol evidence rather than
    # failing. The names are in each run's .log ("pointer-canonicalized ...").
    canon = {k: v.ptr_canon for k, v in runs.items()}
    if len(set(canon.values())) != 1:
        log(f"WARNING scenario {name}: the three runs canonicalized different "
            f"numbers of whole-pointer statics ({canon}) — grep the per-run "
            f".log files in {outdir} for 'pointer-canonicalized' to see which "
            f"symbol differs, and check whether it is a genuine NULL/non-NULL "
            f"transition before treating it as benign")

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


def reconcile_cross_scenario_noise(report):
    """Reclassify DIVERGENT rows that another scenario's baselines proved to be
    baseline-nondeterministic. Mutates `report` in place (row verdicts and each
    scenario's `divergent` count) and returns [(scenario, row, proof)].

    Split out of main() so it can be exercised directly against a stored
    report.json — see the #65 write-up in docs/rollback-determinism-harness.md
    for why "it did not reproduce" is not an acceptable test for this.
    """
    global_noise = set()
    for sc in report["scenarios"]:
        global_noise.update(sc.get("noise_symbols", []))

    reconciled = []
    for sc in report["scenarios"]:
        own_noise = set(sc.get("noise_symbols", []))
        for row in sc.get("rows", []):
            # DIVERGENT only: DIVERGENT+FEEDBACK is never reconciled away.
            if row["verdict"] != "DIVERGENT":
                continue
            if row["symbol"] in own_noise or row["symbol"] not in global_noise:
                continue
            proof = sorted(o["name"] for o in report["scenarios"]
                           if row["symbol"] in set(o.get("noise_symbols", [])))
            row["verdict"] = "UNSTABLE"
            row["reason"] = ("baseline-nondeterministic in " + ", ".join(proof) +
                             " — this scenario's two baselines coincided")
            sc["divergent"] -= 1
            reconciled.append((sc["name"], row, proof))
    return reconciled


# --- main ------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default=os.path.join(
        REPO_ROOT, "build", "host", "3S-ARM.app", "Contents", "MacOS", "3S-ARM"))
    ap.add_argument("--mode", choices=["fast", "thorough"], default="fast")
    ap.add_argument("--frames", type=int, default=None,
                    help="frames per run (default: 1500 fast, 2400 thorough)")
    ap.add_argument("--rollback-period", type=int, default=1,
                    help="in-game cycle period (default 1 = every frame)")
    ap.add_argument("--rollback-depth", type=int, default=3,
                    help="in-game speculative depth (default 3)")
    # Character-select knobs. These are game flags, but the driver now owns
    # them and passes them explicitly (task #63) instead of letting the gate
    # inherit whatever src/main.c happens to default to.
    #
    # Depth 8 is production's GekkoNet input_prediction_window default
    # (netplay.c:903-905) — the gate used to run select at 2, a quarter of the
    # real window, which is enough to certify a fix whose load-bearing half
    # has been deleted (the task-50 duplicate-load leak changes which guard
    # matters between depth 2 and depth >= 3).
    #
    # Period stays at 8 and is NOT a stand-in for depth: it is the documented
    # dodge for the crash-class ppg asset-setup traps in known limit 1, and
    # period 1 is a separately-tracked OPEN RED. Depth and cadence bound
    # different things.
    ap.add_argument("--select-rollback-period", type=int, default=8,
                    help="character-select cycle period (default 8; 0 disables "
                         "select-phase cycles)")
    ap.add_argument("--select-rollback-depth", type=int, default=8,
                    help="character-select speculative depth (default 8 = "
                         "production's input_prediction_window). Independent of "
                         "--rollback-depth.")
    ap.add_argument("--timeout", type=int, default=900, help="per-run timeout (s)")
    ap.add_argument("--outdir", default=None, help="work dir (default: mktemp)")
    ap.add_argument("--allowlist", default=os.path.join(SCRIPT_DIR, "allowlist.txt"))
    # Passthrough for game-side flags the driver does not model. Appended to
    # every run of every scenario, baselines included, so A1/A2/B stay
    # comparable.
    ap.add_argument("--game-arg", action="append", default=[],
                    help="extra argument passed verbatim to the game binary "
                         "(repeatable; applied to A1, A2 and B alike)")
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
              "select_rollback_period": args.select_rollback_period,
              "select_rollback_depth": args.select_rollback_depth,
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
        total_noise += scenario_result["noise"]
        total_allowlisted += scenario_result["allowlisted"]

    # --- cross-scenario noise reconciliation (task #65) --------------------
    #
    # The per-scenario noise control is TWO samples of "is this symbol
    # nondeterministic between processes". Two samples is enough for state
    # that always differs, and not enough for state that differs *most* of
    # the time — which is what wall-clock-driven state does. Measured: over
    # four fast-mode invocations of the same binary, `bgm_exe`
    # (BGMExecution, sound3rd.c:44 — eleven s16/u16 fields, NO pointers, so
    # the capture's pointer canonicalization cannot help it) came out NOISE
    # in three and DIVERGENT in one. It is driven by BGM_Server, a
    # once-per-real-frame global (known limit 8), so its baselines usually
    # disagree and occasionally coincide. That is a false FAIL of exactly
    # the shape #65 is about.
    #
    # The run already holds the evidence to settle it and was throwing it
    # away: every scenario contributes its own pair of baselines, and a
    # symbol that is baseline-nondeterministic is so because of how the
    # process runs, not because of which characters were picked. So a
    # symbol another scenario's baselines PROVED nondeterministic is not
    # reported as a finding here. This uses more of the run's own measured
    # evidence than the two samples the verdict used before; it is not an
    # allowlist (no permanence, no per-symbol reason, nothing hand-written)
    # and nothing is hidden — every reconciled row is printed below with
    # both sides of the evidence and counted as `unstable=` in the summary.
    #
    # HONEST BOUND, because this one is a probability reduction and NOT the
    # structural argument that covers address-valued symbols: with the two
    # fast scenarios it takes four coinciding baselines instead of two to
    # produce a false FAIL. It does not make that impossible. Wall-clock
    # state has no structural test — you cannot canonicalize a timer without
    # destroying the signal — so this class is narrowed, not closed.
    #
    # DIVERGENT+FEEDBACK is deliberately NOT reconciled. A GS_SAVE-covered
    # symbol drifting is the sharpest signal this harness produces, and
    # run_scenario already warns separately when a saved symbol is
    # baseline-noisy. Losing one of those to a cross-scenario inference is a
    # worse trade than a rerun.
    reconciled = reconcile_cross_scenario_noise(report)

    if reconciled:
        print("\n=== cross-scenario noise reconciliation ===")
        for scen, row, proof in reconciled:
            print(f"  UNSTABLE           {row['symbol']:<40} addr={row['addr']} "
                  f"size={row['size']} first={row['first_frame']} "
                  f"last={row['last_frame']} frames={row['divergent_frames']}")
            print(f"    diverged in {scen}; proven baseline-nondeterministic by "
                  f"{', '.join(proof)}")
        print("  These are NOT counted as findings. If a symbol you expected to be "
              "a real finding\n  shows up here, that is a signal in itself — it "
              "means it is also baseline-noisy,\n  so this harness cannot "
              "adjudicate it (known limit 2).")

    for sc in report["scenarios"]:
        total_divergent += sc.get("divergent", 0)
        total_feedback += sc.get("feedback", 0)

    report["unstable"] = len(reconciled)

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
          f"select_period={args.select_rollback_period} "
          f"select_depth={args.select_rollback_depth} "
          f"divergent={total_divergent} feedback={total_feedback} "
          f"allowlisted={total_allowlisted} noise={total_noise} "
          f"unstable={report['unstable']} "
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
