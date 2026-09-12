#!/usr/bin/env python3
"""A private THIRDSARM_HOME for every oracle process a sweep starts.

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
inherited by the child as before and this module does not need a donor at all.
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path

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


def missing_reason(donor: Path | None) -> str | None:
    """A one-line explanation when a sweep is about to run with no romset, or
    None when it is fine.

    A sweep that resolves PS2 balance does not fail -- it reports every archive
    `divergent`, which is indistinguishable from a real engine finding.  That
    must be a loud refusal before the first run, never a result.

    So this checks the thing that actually matters: that a romset will be
    resolvable from inside a hermetic home.  Checking merely that `roms/` or
    `resources/` exists is not enough -- the directory search never looks in
    `resources/`, so a donor with only `resources/sfiii3nr1.zip` (no `roms/`
    symlink beside it) passes that test and still resolves PS2.
    """
    if os.environ.get("THIRDSARM_CPS3_ZIP"):
        return None
    if donor is None:
        return ("no donor pref directory found (looked in "
                + ", ".join(str(c) for c in _PREF_CANDIDATES)
                + ") -- a hermetic home would resolve PS2 balance and report "
                  "every archive divergent. Set $THIRDSARM_DONOR_HOME or "
                  "$THIRDSARM_CPS3_ZIP.")

    # The paths the game will probe inside a seeded home, mapped back to the
    # donor: <home>/roms/<name> -> <donor>/roms/<name>, and <home>/<name>,
    # which a seeded home never has (only roms/ and resources/ are linked).
    if not any((donor / "roms" / name).is_file() for name in _ROM_ZIP_NAMES):
        return (f"donor {donor} has no roms/{{{', '.join(_ROM_ZIP_NAMES)}}} -- "
                "a hermetic home would resolve PS2 balance and report every "
                "archive divergent. Note that resources/ is NOT searched: the "
                "romset must be reachable as roms/<name> (a symlink is fine). "
                "Or set $THIRDSARM_CPS3_ZIP.")
    return None
