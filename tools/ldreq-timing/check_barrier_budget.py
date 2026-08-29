#!/usr/bin/env python3
"""Check the LDREQ barrier budget against GekkoNet's disconnect timeout.

gd3rd.h derives LDREQ_BARRIER_BUDGET_MS from a constant in a third-party
header:

    BUDGET_MS bounds a slow or stuck *disk*: it must stay comfortably
    under GekkoNet's NetStats::DISCONNECT_TIMEOUT of 5000 ms
    (third_party/GekkoNet/build/include/net.h:125), which the remote peer
    measures from the last packet of any type it received from us -- and we
    send nothing while blocked here.

That is a real coupling and a silent one. The barrier blocks the frame loop,
so while it runs we transmit nothing; if the budget ever reaches the peer's
disconnect timeout, a slow disk stops being a stall and becomes a dropped
match. Nothing in the build can catch that, because net.h is a C++ header
(namespace Gekko, <memory>, <vector>) and every consumer of the budget is C
-- no translation unit can see both constants, so a _Static_assert is not
available here. DISCONNECT_TIMEOUT also lives in vendored code that
build-deps.sh re-fetches, so it can change without anyone touching this repo.

This script reads BOTH values from their real definitions. Neither is
restated here, so there is no mirrored constant to fall out of date -- the
failure this check exists to prevent cannot be reintroduced by the check
itself.

Exit codes: 0 = the relationship holds, 1 = it is violated, 2 = a value
could not be located (which is itself a failure -- a check that silently
finds nothing is worse than no check).
"""

import argparse
import pathlib
import re
import sys

# "comfortably under" from the gd3rd.h comment, made concrete: the budget may
# not consume more than this fraction of the peer's disconnect timeout. At the
# shipped 3000/5000 the ratio is 0.60. The margin exists because the peer's
# clock starts at the LAST PACKET IT RECEIVED, not when we entered the
# barrier, so some of its timeout is already spent before we block at all.
MAX_BUDGET_FRACTION = 0.75

DEFAULT_GD3RD_H = "src/sf33rd/Source/Game/io/gd3rd.h"
DEFAULT_NET_H = "third_party/GekkoNet/build/include/net.h"


def find_one(path, pattern, what):
    """Return the single integer `pattern` captures in `path`.

    Requiring exactly one match is deliberate: zero means the definition moved
    or was renamed, several means the regex is no longer specific enough. Both
    make the comparison meaningless, so both are hard errors rather than a
    guess at which match was intended.
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        print(f"[barrier-budget] FAIL: cannot read {path}: {exc}", file=sys.stderr)
        return None, None

    matches = [
        (m.group(1), text.count("\n", 0, m.start()) + 1)
        for m in re.finditer(pattern, text, re.MULTILINE)
    ]
    if len(matches) != 1:
        print(
            f"[barrier-budget] FAIL: expected exactly one definition of {what} "
            f"in {path}, found {len(matches)}. The definition moved, was "
            f"renamed, or was duplicated; this check can no longer prove the "
            f"gd3rd.h claim and must be repaired, not skipped.",
            file=sys.stderr,
        )
        return None, None
    value, line = matches[0]
    return int(value), line


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", default=None)
    ap.add_argument("--gd3rd-h", default=None)
    ap.add_argument("--net-h", default=None)
    args = ap.parse_args()

    root = pathlib.Path(
        args.repo_root or pathlib.Path(__file__).resolve().parents[2]
    )
    gd3rd_h = pathlib.Path(args.gd3rd_h) if args.gd3rd_h else root / DEFAULT_GD3RD_H
    net_h = pathlib.Path(args.net_h) if args.net_h else root / DEFAULT_NET_H

    budget, budget_line = find_one(
        gd3rd_h,
        r"^\s*#define\s+LDREQ_BARRIER_BUDGET_MS\s+(\d+)\s*$",
        "LDREQ_BARRIER_BUDGET_MS",
    )
    timeout, timeout_line = find_one(
        net_h,
        r"^\s*static\s+const\s+u64\s+DISCONNECT_TIMEOUT\s*=\s*(\d+)\s*;",
        "NetStats::DISCONNECT_TIMEOUT",
    )
    if budget is None or timeout is None:
        return 2

    limit = timeout * MAX_BUDGET_FRACTION
    ratio = budget / timeout

    print(
        f"[barrier-budget] LDREQ_BARRIER_BUDGET_MS={budget} "
        f"({gd3rd_h}:{budget_line})"
    )
    print(
        f"[barrier-budget] NetStats::DISCONNECT_TIMEOUT={timeout} "
        f"({net_h}:{timeout_line})"
    )
    print(
        f"[barrier-budget] ratio={ratio:.2f} limit={MAX_BUDGET_FRACTION:.2f} "
        f"({limit:.0f} ms)"
    )

    if budget > limit:
        print(
            f"[barrier-budget] FAIL: the barrier may block for {budget} ms "
            f"while the peer disconnects us after {timeout} ms of silence. "
            f"gd3rd.h states BUDGET_MS 'must stay comfortably under' that "
            f"timeout; at {ratio:.0%} it does not. Either lower "
            f"LDREQ_BARRIER_BUDGET_MS or re-derive the claim in gd3rd.h.",
            file=sys.stderr,
        )
        return 1

    print("[barrier-budget] PASS: barrier budget stays comfortably under the "
          "peer disconnect timeout.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
