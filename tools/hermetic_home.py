#!/usr/bin/env python3
"""A private THIRDSARM_HOME for every game process a harness starts.

Importable by the Python harnesses (`child_env(seed_home(...))`) and runnable
by the shell ones:

    THIRDSARM_HOME="$(python3 tools/hermetic_home.py --seed "$RUNDIR/home")"

so `tools/frame-data/run.sh` gets exactly the behaviour this module implements
instead of a second copy of it in bash.  `--seed` refuses, loudly and nonzero,
in every case where the home it would hand back is unusable; pass
`--no-romset` when the caller does not need arcade balance (a PS2-balance
frame-data corpus), which drops the romset half of that refusal and keeps the
liveness half.

WHY
---
`Paths_GetPrefPath()` (src/port/paths.c) honours `$THIRDSARM_HOME` on every
port, and with it unset the host build resolves `SDL_GetPrefPath()` -- one
directory shared by every 3S-ARM process on the machine, the maintainer's real
one.  `tools/gates/run-gates.sh` has handed each harness a private home since
task #125 (`h_home="${out_dir}/home/$$-${h}"`); the corpus sweep never did.

That is not a tidiness point.  The statcheck oracle reads the settings block out
of `saves/settings` at boot, so the verdict a sweep records is a function of
whoever last touched the maintainer's options screen.  MEASURED (2026-09-07,
docs/savestates-and-instant-mode-jump.md, the 2026-09-07 row): with a rebound
`Pad_Infor` in that home, ten rows recorded `pass` in `manifest.json` came back
rc 1 `divergent` at frames 7 / 12 / 17 / 105 with a CLEAN seed -- a false
engine-divergence verdict on an archive that is fine.  `Playback_Settings_Pin()`
(sys_sub.c) now re-asserts the settings-derived gameplay set every tick, which
removes that symptom, but the exposure stands for every field nobody has thought
to pin yet.  A private home removes the exposure instead of the symptom.

WHAT A HERMETIC HOME NEEDS
--------------------------
Empty, with two exceptions, both read-only:

* `roms/` -- balance.  `StatcheckRunner_PinConfig()` pins `CFG_KEY_BALANCE` to
  "auto", and `auto` resolves to ARCADE only if `ArcadeCharData` can find a
  verified CPS3 romset.  An empty home finds none, silently resolves PS2, and
  desyncs every archive from frame 1: the sweep would not be hermetic, it would
  be wrong.  So the romset is linked in.

* `resources/` -- liveness.  A home with no `SF33RD.AFS` reaches
  `MAIN_PHASE_COPYING_RESOURCES` and blocks in `Resources_RunResourceCopyingFlow()`,
  a modal dialog `SDL_VIDEODRIVER=dummy` makes invisible.  The run does not
  fail, it hangs until the sweep's timeout.  Same reasoning, same fix, as
  `link_resources()` in run-gates.sh.

Everything else -- `config`, `keymap`, `saves/`, `logs/`, `training`, `trials`,
`balance.status` -- is deliberately ABSENT, so each oracle process boots from
`Game_Default_Data` and cannot read, or write, the maintainer's state.

WHY LINK THE ROMSET RATHER THAN SET $THIRDSARM_CPS3_ZIP
-------------------------------------------------------
`$THIRDSARM_CPS3_ZIP` is the documented dev hook and it does resolve the same
romset -- but it is a DIFFERENT BRANCH of `load_cps3_char_data()`
(src/arcade/arcade_char_data.c): the env branch additionally calls
`Cps3FirstLight_TryLoad(env_path)`, which the directory search never does.  The
standing 447/447 was measured through the directory search, and the whole point
of this change is that no verdict may move because of it, so the hermetic home
reproduces that branch exactly -- a `roms/sfiii3nr1.zip` under the pref
directory, which is how the maintainer's own home resolves it.

`$THIRDSARM_CPS3_ZIP` is still honoured: if the caller has it set, it is
inherited by the child and resolves the romset through the env branch.  It does
NOT excuse the rest of the refusal, though -- see `missing_reason`.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import sys
import zipfile
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent

# SDL_GetPrefPath(ORG, APP) with the ORG/APP from src/port/paths.c, on the two
# host platforms a sweep is ever run from.
_PREF_CANDIDATES = (
    Path.home() / "Library/Application Support/CrowdedStreet/3S-ARM",
    Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share")))
    / "CrowdedStreet/3S-ARM",
)

# Linked, never copied, and never written to by a statcheck run.
_DONATED = ("roms", "resources")

# arcade_char_data.c -> cps3_rom_zip_names[]. The directory search looks for
# these under the pref directory and under pref/roms (and the executable's own
# directory, which a build tree never has), so a hermetic home resolves a
# romset only through the linked roms/.
_ROM_ZIP_NAMES = ("sfiii3nr1.zip", "sfiii3.zip")

# resources.c -> Resources_GetAFSPath(). A hermetic home reaches
# Resources_Check() only through <home>/resources/SF33RD.AFS: the two
# earlier candidates sit beside the executable, and a build tree has neither.
_AFS_NAME = "SF33RD.AFS"

# rom_load.c -> simm_specs[], read from the source rather than copied, so the
# pinned digests have ONE definition and a retune of that table cannot leave a
# stale copy here silently accepting a romset the game rejects.
_ROM_LOAD_C = _REPO_ROOT / "src/arcade/rom_load.c"
_SIMM_SPEC_RE = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*(0[xX][0-9A-Fa-f]+)\s*,\s*"([0-9a-fA-F]{64})"\s*\}')

# Each slice is 2 MiB (rom_load.c -> ROM_SIMM_SIZE). Not a constant to match --
# the SHA-256 below is what decides, and it decides the size too -- only a
# ceiling, so a hostile or corrupt entry cannot be streamed forever.
_ENTRY_READ_CEILING = 16 * 1024 * 1024


def rom_simm_specs() -> list[tuple[str, int, str]]:
    """`simm_specs[]` as (name, crc32, sha256_hex), parsed out of rom_load.c.

    Raises when the table cannot be read or does not hold exactly four rows:
    this module's refusal is worth nothing if the digests it checks against
    came from a regex that silently matched three of them.
    """
    text = _ROM_LOAD_C.read_text()
    body = re.search(r"simm_specs\[ROM_SIMM_COUNT\]\s*=\s*\{(.*?)\n\}", text,
                     re.DOTALL)
    if body is None:
        raise RuntimeError(f"could not find simm_specs[] in {_ROM_LOAD_C}")
    specs = [(m.group(1), int(m.group(2), 16), m.group(3).lower())
             for m in _SIMM_SPEC_RE.finditer(body.group(1))]
    if len(specs) != 4:
        raise RuntimeError(
            f"expected 4 rows in {_ROM_LOAD_C}'s simm_specs[], parsed "
            f"{len(specs)} -- the table's shape changed")
    return specs


def romset_reason(zip_path: Path) -> str | None:
    """Why `zip_path` is not a romset the game would accept, or None when it is.

    `Rom_Load` in rom_load.c is the authority, and this is its pipeline: a
    cheap CRC32 pre-filter over the zip's central directory to pick candidate
    entries, then SHA-256 over each candidate's decompressed bytes against the
    pinned digest.  Entries are matched by CONTENT, never by name, for the
    reason that file records -- the merged MAME set carries same-named SIMMs
    with different bytes.

    Existence is NOT the check.  A wrong-revision or truncated zip exists,
    passes any `is_file()` test, and then loses to `Rom_Load`'s verification
    inside the game, where the consequence is an SDL_LogWarn nobody reads and a
    SILENT fall back to PS2 balance -- the exact outcome this refusal exists to
    prevent.
    """
    if not zip_path.is_file():
        return f"{zip_path} is not a file"

    specs = rom_simm_specs()
    want = {crc: (name, sha) for name, crc, sha in specs}

    try:
        with zipfile.ZipFile(zip_path) as zf:
            # crc32 + uncompressed size come from the central directory, so
            # this narrowing costs no decompression even on a ~95 MB set.
            matched: dict[int, bool] = {}
            for info in zf.infolist():
                if info.CRC not in want or info.CRC in matched:
                    continue
                if info.file_size > _ENTRY_READ_CEILING:
                    continue
                digest = hashlib.sha256()
                with zf.open(info) as entry:
                    read = 0
                    while chunk := entry.read(1024 * 64):
                        read += len(chunk)
                        if read > _ENTRY_READ_CEILING:
                            break
                        digest.update(chunk)
                if digest.hexdigest() == want[info.CRC][1]:
                    matched[info.CRC] = True
    except (zipfile.BadZipFile, OSError) as exc:
        return f"{zip_path} could not be read as a zip ({exc})"

    missing = [name for name, crc, _ in specs if crc not in matched]
    if missing:
        return (f"{zip_path} is not the sfiii3nr1 romset the engine accepts: "
                f"{len(missing)} of the 4 SIMM1 slices did not verify "
                f"({', '.join(missing)}). Rom_Load matches by SHA-256, so this "
                "is a WRONG REVISION or a corrupt file, not a missing one.")
    return None


def donor_home() -> Path | None:
    """The host's real pref directory, if one exists, else None.

    Only ever read from: `seed_home` links its `roms/` and `resources/` into
    each hermetic home.  `$THIRDSARM_DONOR_HOME` overrides it for a machine
    whose romset lives somewhere else.
    """
    override = os.environ.get("THIRDSARM_DONOR_HOME")
    if override:
        path = Path(override)
        return path if path.is_dir() else None

    for candidate in _PREF_CANDIDATES:
        if candidate.is_dir():
            return candidate
    return None


def seed_home(home: Path, donor: Path | None = None) -> Path:
    """Create `home` as an empty hermetic THIRDSARM_HOME and return it.

    Idempotent: re-seeding an existing home replaces the two symlinks and
    leaves whatever the previous run wrote (its `logs/`, its `config`) alone,
    because a caller that wants a clean home passes a fresh path.
    """
    if donor is None:
        donor = donor_home()

    home.mkdir(parents=True, exist_ok=True)

    if donor is not None:
        for name in _DONATED:
            src = donor / name
            if not src.is_dir():
                continue
            dst = home / name
            # ln -sfn: a plain symlink() onto an existing symlink-to-directory
            # creates `dst/<name>` instead of replacing `dst`.
            if dst.is_symlink() or dst.exists():
                if dst.is_symlink() or dst.is_file():
                    dst.unlink()
                else:
                    shutil.rmtree(dst)
            dst.symlink_to(src, target_is_directory=True)

    return home


def child_env(home: Path, base: dict[str, str] | None = None) -> dict[str, str]:
    """The environment an oracle child should run with: `base` (default the
    caller's own) plus `THIRDSARM_HOME`, plus the headless video/audio drivers
    every sweep already sets."""
    env = dict(os.environ if base is None else base)
    env["THIRDSARM_HOME"] = str(home)
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"
    return env


def _romset_missing_reason(donor: Path | None) -> str | None:
    """Why arcade balance would not resolve from inside a seeded home."""
    env_zip = os.environ.get("THIRDSARM_CPS3_ZIP")
    if env_zip:
        # Set BUT UNUSABLE is the case this used to wave through: the old code
        # returned None the moment the variable was non-empty, so a typo'd path
        # or a wrong-revision zip bought a silent PS2 run.
        why = romset_reason(Path(env_zip))
        return None if why is None else f"$THIRDSARM_CPS3_ZIP: {why}"

    if donor is None:
        return ("no donor pref directory found (looked in "
                + ", ".join(str(c) for c in _PREF_CANDIDATES)
                + ") -- a hermetic home would resolve PS2 balance. Set "
                  "$THIRDSARM_DONOR_HOME or $THIRDSARM_CPS3_ZIP.")

    # The paths the game will probe inside a seeded home, mapped back to the
    # donor: <home>/roms/<name> -> <donor>/roms/<name>. <home>/<name> is never
    # searched successfully, because only roms/ and resources/ are linked.
    present = [donor / "roms" / name for name in _ROM_ZIP_NAMES
               if (donor / "roms" / name).is_file()]
    if not present:
        return (f"donor {donor} has no roms/{{{', '.join(_ROM_ZIP_NAMES)}}} -- "
                "a hermetic home would resolve PS2 balance. Note that "
                "resources/ is NOT searched: the romset must be reachable as "
                "roms/<name> (a symlink is fine). Or set $THIRDSARM_CPS3_ZIP.")

    whys = [romset_reason(path) for path in present]
    if all(why is not None for why in whys):
        return "; ".join(why for why in whys if why is not None)
    return None


def _liveness_missing_reason(donor: Path | None) -> str | None:
    """Why a seeded home would never leave MAIN_PHASE_COPYING_RESOURCES."""
    if donor is not None and (donor / "resources" / _AFS_NAME).is_file():
        return None
    where = ("there is no donor pref directory to supply"
             if donor is None else f"donor {donor} has no")
    return (f"{where} resources/{_AFS_NAME}, and `seed_home` links "
            f"resources/ from the donor and nowhere else. Without it "
            f"`Resources_Check()` fails, `main.c` enters "
            f"MAIN_PHASE_COPYING_RESOURCES, and the run HANGS in "
            f"`Resources_RunResourceCopyingFlow()`'s file dialog, which "
            f"SDL_VIDEODRIVER=dummy makes invisible. Set "
            f"$THIRDSARM_DONOR_HOME to a directory that has it.")


def missing_reason(donor: Path | None, *, need_romset: bool = True) -> str | None:
    """Every reason a hermetic home built from `donor` would misreport, joined,
    or None when it would be sound.

    Two independent failure modes, and BOTH are silent, which is why they are
    refused up front rather than left to be read out of a result:

    * romset -- a run that resolves PS2 balance where arcade was wanted does
      not fail. A sweep reports every archive `divergent`, indistinguishable
      from a real engine finding; a frame-data corpus measures the wrong engine
      and can still be green. Pass `need_romset=False` only when the caller
      genuinely wants PS2 (a `balance: ps2` corpus), never to get past a
      refusal.
    * liveness -- a home with no `resources/SF33RD.AFS` does not fail either.
      It hangs until the caller's timeout, which reads as an engine hang.
    """
    reasons = []
    if need_romset:
        reasons.append(_romset_missing_reason(donor))
    reasons.append(_liveness_missing_reason(donor))
    joined = "; ".join(reason for reason in reasons if reason is not None)
    return joined or None


def _cli(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Seed a hermetic THIRDSARM_HOME and print its path.")
    ap.add_argument("--seed", metavar="DIR", required=True,
                    help="directory to create the home at (created if absent)")
    ap.add_argument("--no-romset", action="store_true",
                    help="the caller does not need arcade balance, so a "
                         "missing or unverifiable CPS3 romset is not a refusal "
                         "(the resources/ liveness check still is)")
    args = ap.parse_args(argv)

    donor = donor_home()
    reason = missing_reason(donor, need_romset=not args.no_romset)
    if reason is not None:
        print(f"error: refusing to seed a hermetic THIRDSARM_HOME: {reason}",
              file=sys.stderr)
        return 1

    home = seed_home(Path(args.seed), donor)
    print(f"[hermetic_home] THIRDSARM_HOME={home} (romset donor: {donor})",
          file=sys.stderr)
    print(str(home))
    return 0


if __name__ == "__main__":
    raise SystemExit(_cli())
