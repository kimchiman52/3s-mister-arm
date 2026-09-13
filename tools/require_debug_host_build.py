#!/usr/bin/env python3
"""`require_debug_host_build` for the Python harnesses.

`tools/require-debug-host-build.sh` guards the four SHELL entry points that
build `build/host`. The Python harnesses that drive the same directory --
`tools/frame-data/check-select-timer.py`, `check_rollback_determinism.py` when
run on its `--binary` default, and `tools/ldreq-timing/rollback_plt_req_probe.py`
-- had no guard at all, so a `build/host` somebody configured Release gave them
a binary with no `--test-*` hooks compiled in: it boots a normal game, never
reaches the scripted exit, and is killed by the wall-clock cap. The shell
guard's header records two lanes that each lost about half an hour to that
misdiagnosis.

This is a SHIM, not a second predicate. It shells out to the bash function so
the rules (absent is fine, Debug is fine, ENABLE_DEBUG_HOOKS is fine, a
multi-config generator is fine) have one definition; duplicating them in Python
is how the two copies would come to disagree.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

_SHELL_GUARD = Path(__file__).resolve().parent / "require-debug-host-build.sh"


def enclosing_build_dir(binary: str | os.PathLike[str]) -> Path | None:
    """The CMake build directory `binary` was produced in, or None.

    Walks up from the binary looking for a CMakeCache.txt, so a caller that
    points `--binary` at some other tree is checked against THAT tree rather
    than against `build/host`, and one that points at a loose binary in no
    build directory is not checked at all.
    """
    for parent in Path(binary).resolve().parents:
        if (parent / "CMakeCache.txt").is_file():
            return parent
    return None


def require_debug_host_build(binary: str | os.PathLike[str], tag: str) -> bool:
    """True when `binary`'s build directory carries the `#if DEBUG` harness
    code. On False the bash guard has already printed what is wrong and how to
    fix it, so a caller just needs to stop."""
    build_dir = enclosing_build_dir(binary)
    if build_dir is None:
        return True
    completed = subprocess.run(
        ["bash", "-c",
         'set -e; . "$1"; require_debug_host_build "$2" "$3"',
         "require_debug_host_build", str(_SHELL_GUARD), str(build_dir), tag])
    return completed.returncode == 0
