#!/usr/bin/env python3
"""Acceptance test for tools/hermetic_home.py and tools/require_debug_host_build.py.

WHAT THIS ASSERTS
-----------------
That the refusal actually refuses. Both of the holes it is asserted against
were live in the shipped module and both fail SILENTLY in the game rather than
loudly in the harness, which is the whole reason the checks exist:

  * a donor whose `roms/sfiii3.zip` is corrupt or the wrong revision. The old
    check was `(donor/"roms"/name).is_file()`, so this passed, `Rom_Load`
    then refused the file inside the game, `ArcadeCharData` logged "FAILED
    CONTENT VERIFICATION" into a log nobody reads, and balance resolved PS2 --
    a sweep measuring the wrong engine and still reporting numbers.
  * `$THIRDSARM_CPS3_ZIP` set. The old check returned None the moment that
    variable was non-empty, whatever it pointed at, so with no donor
    `seed_home` linked nothing, `Resources_Check()` failed, and the run hung in
    MAIN_PHASE_COPYING_RESOURCES until the caller's timeout.

The romset ACCEPTANCE path is tested too, and without needing a real romset:
the pinned digests are read from rom_load.c, so the test can build a synthetic
zip, compute its own digests, and assert that a matching zip is accepted and a
one-byte mutation of it is not. A checker that refuses everything would catch
both holes above and be useless; recall and silence are asserted together.

Run:  python3 tools/tests/test_hermetic_home.py
Exit: 0 all assertions hold; 1 an assertion failed; 2 harness failure.
"""

from __future__ import annotations

import contextlib
import hashlib
import os
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

import hermetic_home as hh  # noqa: E402
import require_debug_host_build as rdhb  # noqa: E402

passed = 0
failed = 0


def check(label: str, condition: bool, detail: str = "") -> None:
    global passed, failed
    if condition:
        passed += 1
        print(f"  ok   {label}")
    else:
        failed += 1
        print(f"  FAIL {label}{(' -- ' + detail) if detail else ''}")


def refuses(label: str, reason: str | None, must_mention: str) -> None:
    check(label, reason is not None and must_mention in reason,
          f"reason={reason!r} (expected it to mention {must_mention!r})")


@contextlib.contextmanager
def stderr_muted():
    """The bash guard prints its whole diagnosis on refusal, which is the point
    of it -- and 30 lines of expected output in the middle of a test's own
    report is how a real failure gets skimmed past."""
    saved = os.dup(2)
    with open(os.devnull, "w") as devnull:
        os.dup2(devnull.fileno(), 2)
    try:
        yield
    finally:
        os.dup2(saved, 2)
        os.close(saved)


# --- the digest table is read, not copied ----------------------------------
#
# Every assertion below rests on this parse. If the regex silently matched
# three rows out of four, a zip missing the fourth slice would be accepted.
specs = hh.rom_simm_specs()
check("rom_simm_specs parses 4 rows from rom_load.c", len(specs) == 4,
      f"parsed {len(specs)}")
check("rom_simm_specs digests are 64 hex chars",
      all(len(sha) == 64 for _, _, sha in specs))
check("rom_simm_specs names are the SIMM1 slices",
      sorted(name for name, _, _ in specs)
      == ["sfiii3-simm1.0", "sfiii3-simm1.1", "sfiii3-simm1.2", "sfiii3-simm1.3"])

tmp = Path(tempfile.mkdtemp(prefix="hermetic-home-test-"))


def synthetic_romset(path: Path, bodies: list[bytes]) -> None:
    with zipfile.ZipFile(path, "w") as zf:
        for index, body in enumerate(bodies):
            zf.writestr(f"slice{index}", body)


# A romset whose four entries ARE what the pinned table asks for, with the
# table temporarily pointed at digests computed from these bodies. Entry names
# are deliberately not the canonical ones: Rom_Load matches by content.
bodies = [b"slice-%d-" % i * 64 for i in range(4)]
synthetic_good = tmp / "good.zip"
synthetic_romset(synthetic_good, bodies)
with zipfile.ZipFile(synthetic_good) as zf:
    fake_specs = [(f"sfiii3-simm1.{i}", info.CRC,
                   hashlib.sha256(bodies[i]).hexdigest())
                  for i, info in enumerate(zf.infolist())]

real_specs = hh.rom_simm_specs
hh.rom_simm_specs = lambda: fake_specs
try:
    check("romset_reason accepts a zip whose 4 slices match the pinned digests",
          hh.romset_reason(synthetic_good) is None,
          str(hh.romset_reason(synthetic_good)))

    # One byte changed in one slice. The CRC32 pre-filter alone would drop the
    # entry; the point is that the verdict is a refusal, not a pass.
    mutated = bytearray(bodies[2])
    mutated[0] ^= 0xFF
    synthetic_bad = tmp / "one-byte-off.zip"
    synthetic_romset(synthetic_bad, [bodies[0], bodies[1], bytes(mutated), bodies[3]])
    refuses("romset_reason refuses a one-byte mutation of a good slice",
            hh.romset_reason(synthetic_bad), "sfiii3-simm1.2")

    # Truncated so the whole slice is gone rather than altered.
    synthetic_short = tmp / "three-slices.zip"
    synthetic_romset(synthetic_short, bodies[:3])
    refuses("romset_reason refuses a zip missing one slice",
            hh.romset_reason(synthetic_short), "1 of the 4")
finally:
    hh.rom_simm_specs = real_specs

# --- the two holes, against the real pinned digests ------------------------
not_a_zip = tmp / "roms" / "sfiii3.zip"
not_a_zip.parent.mkdir(parents=True, exist_ok=True)
not_a_zip.write_bytes(os.urandom(4096))
refuses("romset_reason refuses a file that is not a zip at all",
        hh.romset_reason(not_a_zip), "could not be read as a zip")
refuses("romset_reason refuses a path that does not exist",
        hh.romset_reason(tmp / "nope.zip"), "is not a file")

# Hole 1: existence is not verification. This donor has BOTH candidate names
# present as files, which is exactly what the old is_file() check tested.
donor_bad_rom = tmp / "donor-bad-rom"
(donor_bad_rom / "roms").mkdir(parents=True)
(donor_bad_rom / "resources").mkdir(parents=True)
(donor_bad_rom / "resources" / "SF33RD.AFS").write_bytes(b"stand-in")
# Valid zips whose CONTENTS are wrong: that is a wrong-revision romset, the
# case the old is_file() check waved through. (A file that is not a zip at all
# is covered separately above.)
for name in ("sfiii3nr1.zip", "sfiii3.zip"):
    synthetic_romset(donor_bad_rom / "roms" / name, bodies)

saved_env = {k: os.environ.get(k)
             for k in ("THIRDSARM_CPS3_ZIP", "THIRDSARM_DONOR_HOME")}
try:
    os.environ.pop("THIRDSARM_CPS3_ZIP", None)
    refuses("missing_reason refuses a donor whose romset does not verify",
            hh.missing_reason(donor_bad_rom), "WRONG REVISION")
    check("...and does not refuse it when the caller wants no romset",
          hh.missing_reason(donor_bad_rom, need_romset=False) is None,
          str(hh.missing_reason(donor_bad_rom, need_romset=False)))

    # Hole 2: $THIRDSARM_CPS3_ZIP used to short-circuit the whole check.
    os.environ["THIRDSARM_CPS3_ZIP"] = str(tmp / "nope.zip")
    refuses("missing_reason refuses an unusable $THIRDSARM_CPS3_ZIP",
            hh.missing_reason(donor_bad_rom), "THIRDSARM_CPS3_ZIP")
    refuses("missing_reason refuses $THIRDSARM_CPS3_ZIP set with no donor",
            hh.missing_reason(None), "SF33RD.AFS")
    os.environ["THIRDSARM_CPS3_ZIP"] = str(synthetic_good)
    refuses("$THIRDSARM_CPS3_ZIP does not excuse the liveness half",
            hh.missing_reason(None, need_romset=False), "SF33RD.AFS")

    # Liveness: a donor with a romset but no AFS hangs rather than fails, so
    # it is refused up front.
    os.environ.pop("THIRDSARM_CPS3_ZIP", None)
    donor_no_afs = tmp / "donor-no-afs"
    (donor_no_afs / "roms").mkdir(parents=True)
    (donor_no_afs / "resources").mkdir(parents=True)
    refuses("missing_reason refuses a donor with no resources/SF33RD.AFS",
            hh.missing_reason(donor_no_afs, need_romset=False),
            "MAIN_PHASE_COPYING_RESOURCES")

    # --- seed_home -------------------------------------------------------
    home = hh.seed_home(tmp / "home", donor_bad_rom)
    check("seed_home links roms/ and resources/ and creates nothing else",
          sorted(p.name for p in home.iterdir()) == ["resources", "roms"],
          str(sorted(p.name for p in home.iterdir())))
    check("seed_home's donations are symlinks, never copies",
          (home / "roms").is_symlink() and (home / "resources").is_symlink())
    check("seed_home is idempotent",
          hh.seed_home(tmp / "home", donor_bad_rom) == home
          and sorted(p.name for p in home.iterdir()) == ["resources", "roms"])

    # --- child_env -------------------------------------------------------
    env = hh.child_env(home, {"KEEP": "me"})
    check("child_env sets THIRDSARM_HOME to the home",
          env["THIRDSARM_HOME"] == str(home))
    check("child_env sets both headless drivers",
          env["SDL_VIDEODRIVER"] == "dummy" and env["SDL_AUDIODRIVER"] == "dummy")
    check("child_env preserves the base it was handed", env["KEEP"] == "me")
    check("child_env with an explicit base does not re-inherit os.environ",
          "PATH" not in env)

    # --- the CLI the shell harnesses call --------------------------------
    #
    # run.sh does FDH_HOME="$(... --seed ...)" || exit 1. A refusal that wrote
    # the path to stdout anyway would hand it a home it had just refused.
    cli = [sys.executable, str(REPO_ROOT / "tools" / "hermetic_home.py"),
           "--seed", str(tmp / "cli-home")]
    env_no_donor = dict(os.environ, THIRDSARM_DONOR_HOME=str(tmp / "absent"))
    env_no_donor.pop("THIRDSARM_CPS3_ZIP", None)
    refused = subprocess.run(cli, capture_output=True, text=True, env=env_no_donor)
    check("CLI exits nonzero when it refuses", refused.returncode != 0)
    check("CLI writes NOTHING to stdout when it refuses",
          refused.stdout == "", repr(refused.stdout))
    check("CLI explains the refusal on stderr", "error:" in refused.stderr)
    check("CLI does not create the home it refused",
          not (tmp / "cli-home").exists())

    ok = subprocess.run(cli + ["--no-romset"], capture_output=True, text=True,
                        env=dict(env_no_donor,
                                 THIRDSARM_DONOR_HOME=str(donor_bad_rom)))
    check("CLI succeeds on a usable donor", ok.returncode == 0, ok.stderr)
    check("CLI prints the home path, and only that, on stdout",
          ok.stdout.strip() == str(tmp / "cli-home"), repr(ok.stdout))
finally:
    for key, value in saved_env.items():
        if value is None:
            os.environ.pop(key, None)
        else:
            os.environ[key] = value

# --- the require-debug-host-build shim -------------------------------------
#
# It delegates to the bash predicate; what is asserted here is that the
# delegation works and that the build directory is found from the BINARY, which
# is what lets a harness point --binary somewhere else without being checked
# against build/host.
for build_type, expected in (("Debug", True), ("Release", False)):
    tree = tmp / f"tree-{build_type}"
    (tree / "sub").mkdir(parents=True)
    (tree / "CMakeCache.txt").write_text(
        f"CMAKE_BUILD_TYPE:STRING={build_type}\n"
        "CMAKE_GENERATOR:INTERNAL=Unix Makefiles\n")
    binary = tree / "sub" / "3S-ARM"
    binary.write_bytes(b"")
    with stderr_muted():
        verdict = rdhb.require_debug_host_build(binary, "test")
    check(f"shim agrees with the bash guard on a {build_type} tree",
          verdict is expected)
    check(f"shim finds the {build_type} build dir from the binary",
          rdhb.enclosing_build_dir(binary) == tree.resolve())

hooks_tree = tmp / "tree-hooks"
(hooks_tree / "sub").mkdir(parents=True)
(hooks_tree / "CMakeCache.txt").write_text(
    "CMAKE_BUILD_TYPE:STRING=Release\n"
    "CMAKE_GENERATOR:INTERNAL=Unix Makefiles\n"
    "ENABLE_DEBUG_HOOKS:BOOL=ON\n")
(hooks_tree / "sub" / "3S-ARM").write_bytes(b"")
with stderr_muted():
    hooks_verdict = rdhb.require_debug_host_build(hooks_tree / "sub" / "3S-ARM", "test")
check("shim accepts -DENABLE_DEBUG_HOOKS=ON on a Release tree",
      hooks_verdict is True)

loose = tmp / "loose-binary"
loose.write_bytes(b"")
check("shim does not check a binary that is in no build directory",
      rdhb.enclosing_build_dir(loose) is None
      and rdhb.require_debug_host_build(loose, "test") is True)

print(f"\ntest_hermetic_home: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
