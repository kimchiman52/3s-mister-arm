#!/usr/bin/env python3
"""Check the rendezvous server's port-reclaim staleness window against the
CLIENT cadences it is derived from.

TASK #130. Both same-IP port-reclaim arms in rendezvous-server.js used to fire
on "same public address + reclaim budget remaining" with NO staleness
precondition, so they repointed a LIVE slot -- slot A, where the host sits,
included -- and re-notified the paired peer with the claimant's endpoint. The
fix adds a precondition, and the precondition is a MULTIPLE OF THE SLOT'S OWN
OBSERVED CADENCE rather than a fixed window. This script is why that multiple
is a derivation and not a preference.

WHY NOT A FIXED WINDOW. Task #105 -- the reason the reclaim arms exist at all
-- was itself caused by a badly-chosen window: SLOT_STALE_MS is 30 s, sized
against the HOST's 5 s advertise cadence, and applying it to a JOINER slot
refreshed every 500 ms exceeded the joiner's whole 31.8 s connect deadline, so
the retry mechanism was inert in exactly the lossy conditions it existed for.
Any single constant W faces two constraints that do not overlap:

    protect a live host slot     =>  W > host cadence, with loss margin
                                     (one missed 5000 ms refresh => W > 10000)
    keep #105 working            =>  W < the joiner's attempt-2 signalling leg
                                     (signal_budget_ms = 8000)

10000 < W < 8000 is empty. The two slots' occupants do not share a cadence, so
one number cannot serve both. The server therefore MEASURES each slot's
cadence and applies the standard SLOT_STALE_MS already encodes -- six missed
refreshes -- in units of that measurement.

WHAT THIS SCRIPT ENFORCES. Two relationships, every value read from its REAL
definition so nothing is mirrored here and this check cannot itself go stale:

  C1  SLOT_STALE_MS == PORT_RECLAIM_MISSED_REFRESHES x host_register_default
      The factor is not a new magic number -- it IS SLOT_STALE_MS divided by
      the host cadence it was written against (30000 / 5000 = 6). If the two
      stop agreeing, then either the factor was tuned without re-deriving
      SLOT_STALE_MS, or the host cadence moved and the "six missed refreshes"
      standard silently became something else. Both are the #105 defect class.

  C2  PORT_RECLAIM_MISSED_REFRESHES x joiner_race_cadence
        + 2 x STUN_PUNCH_CONFIRM_MS  <=  signal_budget_default
      #105 preservation. The joiner's dead attempt-1 slot has a FROZEN
      lastSeen, so the threshold is crossed while attempt 2 is still
      re-REGISTERing every 500 ms into its signalling leg. The reclaim must
      land early enough in that leg for the DELIVER and the punch to follow;
      2 x STUN_PUNCH_CONFIRM_MS is the design's own allowance for a late
      DELIVER (RACE_HARD_CAP_MS, direct_p2p.c). If this fails, the retry
      lockout #105 closed has been reopened.

WHY A SCRIPT AND NOT AN ASSERTION. Identical to the coupling documented in
check_key_rate_budget.py: the server is a long-lived VPS process and the
client ships in a release ZIP, so they DEPLOY INDEPENDENTLY. No translation
unit and no module sees both a C literal and a JS const, so neither a
_Static_assert nor a JS assertion is available. Prose citations are not a
check.

Exit codes: 0 = both relationships hold, 1 = one is violated, 2 = a value
could not be located (itself a failure -- a check that silently finds nothing
is worse than no check).
"""

import argparse
import pathlib
import re
import sys

DEFAULT_SERVER_JS = "tools/rendezvous-server/rendezvous-server.js"
DEFAULT_CLIENT_C = "src/netplay/direct_p2p.c"
DEFAULT_CONFIG_C = "src/port/config/config.c"
DEFAULT_STUN_H = "src/netplay/stun.h"


def find_one(path, pattern, what, flags=re.MULTILINE):
    """Return the single integer `pattern` captures in `path`, and its line.

    Exactly one match is required. Zero means the definition moved or was
    renamed; several mean the regex is no longer specific enough. Both make
    the comparison meaningless, so both are hard errors rather than a guess at
    which match was intended.
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        print(f"[reclaim-window] FAIL: cannot read {path}: {exc}", file=sys.stderr)
        return None, None

    matches = [
        (m.group(1), text.count("\n", 0, m.end(1)) + 1)
        for m in re.finditer(pattern, text, flags)
    ]
    if len(matches) != 1:
        print(
            f"[reclaim-window] FAIL: expected exactly one definition of {what} "
            f"in {path}, found {len(matches)}. The definition moved, was "
            f"renamed, or was duplicated; this check can no longer prove the "
            f"rendezvous-server.js derivation and must be repaired, not "
            f"skipped.",
            file=sys.stderr,
        )
        return None, None
    value, line = matches[0]
    return int(value), line


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", default=None)
    args = ap.parse_args()

    root = pathlib.Path(
        args.repo_root or pathlib.Path(__file__).resolve().parents[2]
    )
    server_js = root / DEFAULT_SERVER_JS
    client_c = root / DEFAULT_CLIENT_C
    config_c = root / DEFAULT_CONFIG_C
    stun_h = root / DEFAULT_STUN_H

    # --- SERVER side -----------------------------------------------------
    slot_stale_ms, slot_stale_line = find_one(
        server_js,
        r"^const\s+SLOT_STALE_MS\s*=\s*(\d+)\s*\*\s*1000\s*;",
        "SLOT_STALE_MS (in seconds x 1000)",
    )
    if slot_stale_ms is not None:
        slot_stale_ms *= 1000
    missed, missed_line = find_one(
        server_js,
        r"^const\s+PORT_RECLAIM_MISSED_REFRESHES\s*=\s*(\d+)\s*;",
        "PORT_RECLAIM_MISSED_REFRESHES",
    )

    # --- CLIENT side -----------------------------------------------------
    # The host's advertise cadence: the SHIPPED DEFAULT out of the config
    # table, not the 1000 ms floor in direct_p2p.c. The floor is what a user
    # may tune down to; the default is what the derivation is sized against,
    # and it is the value SLOT_STALE_MS's own comment cites.
    host_cadence_ms, host_line = find_one(
        config_c,
        r"CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_INTERVAL_MS\s*,\s*"
        r"\.type\s*=\s*CFG_INT\s*,\s*\.value\.i\s*=\s*(\d+)\s*\}",
        "the host re-REGISTER interval default",
    )
    # The joiner's in-race REGISTER resend. A bare literal in the send gate,
    # not a #define, so the gate expression itself is the anchor. Same anchor
    # check_key_rate_budget.py uses.
    race_cadence_ms, race_line = find_one(
        client_c,
        r"\(\s*now\s*-\s*signal_last_send\s*\)\s*>=\s*(\d+)u",
        "the in-race REGISTER resend cadence",
    )
    # The joiner's attempt-2 signalling leg -- the window the reclaim must
    # land inside. Shipped default from the config table.
    signal_budget_ms, signal_line = find_one(
        config_c,
        r"CFG_KEY_NETPLAY_DIRECT_P2P_SIGNAL_BUDGET_MS\s*,\s*"
        r"\.type\s*=\s*CFG_INT\s*,\s*\.value\.i\s*=\s*(\d+)\s*\}",
        "the signalling-leg budget default",
    )
    confirm_ms, confirm_line = find_one(
        stun_h,
        r"^#define\s+STUN_PUNCH_CONFIRM_MS\s+(\d+)\s*$",
        "STUN_PUNCH_CONFIRM_MS",
    )

    if None in (slot_stale_ms, missed, host_cadence_ms, race_cadence_ms,
                signal_budget_ms, confirm_ms):
        return 2
    if missed <= 1:
        print(
            f"[reclaim-window] FAIL: PORT_RECLAIM_MISSED_REFRESHES is "
            f"{missed}. At 1 the threshold equals the occupant's own cadence, "
            f"so any jitter or a single lost refresh makes a LIVE slot look "
            f"reclaimable and the precondition is decorative. At 0 it is "
            f"absent.",
            file=sys.stderr,
        )
        return 1
    if race_cadence_ms <= 0 or host_cadence_ms <= 0:
        print(
            f"[reclaim-window] FAIL: a client cadence read as "
            f"{race_cadence_ms}/{host_cadence_ms} ms; a zero or negative "
            f"cadence is not a cadence and no multiple of it bounds anything.",
            file=sys.stderr,
        )
        return 1

    print(f"[reclaim-window] server SLOT_STALE_MS={slot_stale_ms} "
          f"({server_js}:{slot_stale_line})")
    print(f"[reclaim-window] server PORT_RECLAIM_MISSED_REFRESHES={missed} "
          f"({server_js}:{missed_line})")
    print(f"[reclaim-window] client host advertise cadence={host_cadence_ms} ms "
          f"({config_c}:{host_line})")
    print(f"[reclaim-window] client in-race REGISTER cadence={race_cadence_ms} ms "
          f"({client_c}:{race_line})")
    print(f"[reclaim-window] client signal-leg budget={signal_budget_ms} ms "
          f"({config_c}:{signal_line})")
    print(f"[reclaim-window] client STUN_PUNCH_CONFIRM_MS={confirm_ms} ms "
          f"({stun_h}:{confirm_line})")

    rc = 0

    # --- C1: the factor IS SLOT_STALE_MS expressed in host refreshes -----
    host_threshold = missed * host_cadence_ms
    print(f"[reclaim-window] C1 host-slot threshold = {missed} x "
          f"{host_cadence_ms} = {host_threshold} ms; SLOT_STALE_MS = "
          f"{slot_stale_ms} ms")
    if host_threshold != slot_stale_ms:
        print(
            f"[reclaim-window] FAIL (C1): a host-cadence slot now resolves to "
            f"{host_threshold} ms, but SLOT_STALE_MS is {slot_stale_ms} ms. "
            f"These must be equal: the whole claim of task #130 is that over "
            f"HOST-cadence slots the port-reclaim arms grant nothing the "
            f"pre-existing staleness rule did not already grant.\n"
            f"[reclaim-window] If they differ upward, the arms are strictly "
            f"weaker than SLOT_STALE_MS and are dead code pretending to be a "
            f"control. If downward, a LIVE host slot is reclaimable below the "
            f"staleness bar again -- which is the #130 finding, reopened.\n"
            f"[reclaim-window] Re-derive: either set "
            f"PORT_RECLAIM_MISSED_REFRESHES = SLOT_STALE_MS / host cadence, "
            f"or move SLOT_STALE_MS with the cadence. Do not just silence "
            f"this.",
            file=sys.stderr,
        )
        rc = 1

    # --- C2: #105 must keep working --------------------------------------
    joiner_threshold = missed * race_cadence_ms
    punch_allowance = 2 * confirm_ms
    needed = joiner_threshold + punch_allowance
    print(f"[reclaim-window] C2 joiner-slot threshold = {missed} x "
          f"{race_cadence_ms} = {joiner_threshold} ms; + 2 x {confirm_ms} ms "
          f"punch allowance = {needed} ms; signal leg = {signal_budget_ms} ms")
    if needed > signal_budget_ms:
        print(
            f"[reclaim-window] FAIL (C2): a joiner-cadence slot only becomes "
            f"reclaimable after {joiner_threshold} ms, and with the "
            f"{punch_allowance} ms punch allowance that needs "
            f"{needed} ms -- more than the {signal_budget_ms} ms attempt-2 "
            f"signalling leg the retry actually has.\n"
            f"[reclaim-window] That is task #105 REOPENED, in its original "
            f"form: a staleness bar the joiner's retry cannot outlast, so the "
            f"retry is ignored for the whole connect budget and the user sees "
            f"a failed join in exactly the lossy conditions the retry exists "
            f"for.\n"
            f"[reclaim-window] The server DEPLOYS SEPARATELY from the client, "
            f"so this is not a build break -- it is a production lockout.",
            file=sys.stderr,
        )
        rc = 1

    if rc == 0:
        print("[reclaim-window] PASS: the reclaim threshold still matches "
              "SLOT_STALE_MS at host cadence and still fits the joiner's "
              "retry leg.")
    return rc


if __name__ == "__main__":
    sys.exit(main())
