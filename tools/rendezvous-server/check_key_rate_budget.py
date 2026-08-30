#!/usr/bin/env python3
"""Check the rendezvous server's per-key rate cap against the CLIENT cadences
it is derived from.

rendezvous-server.js derives KEY_RATE_LIMIT_PER_WINDOW from constants that
live in the C client, and embeds them by file:line in a prose derivation:

    legit peak  = host 1 + 2 x N = 1 + 2 x 6            = 13/s
    per-key cap = RATE_LIMIT_PER_WINDOW + legit peak    => >= 23
    ...the smallest integer k satisfying that is k = 3  => 30

Both halves of that are enforced here: the absorption floor AND the
integrality of k (task #131 -- enforcing only the former left every cap in
[23, 29] passing while the server's stated 3:1 ratio and 1.5x under-attack
headroom were false).

The three inputs to that arithmetic are:

  * the joiner's in-race REGISTER resend cadence (500 ms => 2/s per dialer),
    a bare literal inside p2p_race's signalling leg;
  * the host re-REGISTER worker's interval FLOOR (1000 ms => 1/s), which is
    the fastest a legitimate host can charge the bucket; and
  * N, the number of simultaneous dialers on one key the cap is sized for.

That is a real coupling and a silent one. The client and the server DEPLOY
INDEPENDENTLY -- the server is a long-lived VPS process, the client ships in
a release ZIP -- so a client cadence tune does not rebuild, restart, or even
notify the server. Nothing in either build can catch it: no translation unit
and no module sees both a C literal and a JS const, so neither a
_Static_assert nor a JS assertion is available here. The prose derivation and
its file:line citations are the ONLY thing holding the two together, and a
citation is not a check.

What goes wrong when it drifts is not a crash. If the client's cadence gets
faster (or N grows), the legitimate peak of a full room climbs past what the
deployed cap admits, and the room DoSes ITSELF with no attacker present: the
host's liveness REGISTERs are rate-dropped, entry.lastSeenA stops refreshing,
and SLOT_STALE_MS / SESSION_TTL_MS reclaim a room whose code is still on the
host's screen. The user sees a code that simply stops working.

This script reads EVERY value from its real definition -- the two C literals
out of direct_p2p.c, the three caps out of rendezvous-server.js. Nothing is
restated here, so there is no mirrored constant that can itself fall out of
date, and the failure this check exists to prevent cannot be reintroduced by
the check.

Modelled on tools/ldreq-timing/check_barrier_budget.py, which does the same
job for the C-vs-C++ LDREQ barrier pair.

Exit codes: 0 = the relationship holds, 1 = it is violated, 2 = a value could
not be located (which is itself a failure -- a check that silently finds
nothing is worse than no check).
"""

import argparse
import math
import pathlib
import re
import sys

# The derivation is written in requests-per-SECOND. That is only the same
# thing as requests-per-WINDOW because the window is one second, so the window
# is read and asserted rather than assumed -- the same guard
# testKeyBudgetCoversMultiJoinerRoom applies on the JS side.
REQUIRED_WINDOW_MS = 1000

# Constraint 1 from the derivation: the per-key cap must be an INTEGER
# multiple k of the per-IP cap, with k at least this. At k = 1 the per-key
# limiter adds ZERO attacker cost over the per-IP limiter -- that IP is
# already admitted at the per-IP rate -- while handing that one IP the power
# to drop every other frame on the key, host liveness included. It stops being
# a defense and becomes a lockout weapon.
#
# THE INTEGRALITY IS PART OF THE CONSTRAINT, NOT A ROUNDING CONVENIENCE
# (task #131). This gate originally enforced the ratio as the LINEAR bound
# `k * ip_cap`, which is weaker than the design it guards: with the shipped
# values it required 23 while the server's derivation
# (rendezvous-server.js, "Constraint 1"/"Constraint 2") states an integer
# k >= 2 and therefore k = 3 => 30. Every cap in [23, 29] passed the gate
# while making the server's own stated margins false -- at 23 the
# under-attack headroom is 13 of 13, i.e. exactly zero, against the 1.5x
# the derivation claims. The design is the authority and the gate was the
# transcription that dropped a clause: this file's own docstring already
# quotes "the smallest integer k satisfying that is k = 3", so the two
# halves of THIS file disagreed too.
#
# The upper half of the same fence lives in __test_protocol.js
# ("k = 3 is the SMALLEST integer factor", cap <= 3 x per-IP). With the
# integrality enforced here the pair pins the cap to exactly 30 rather than
# to a nine-wide band.
MIN_KEY_TO_IP_RATIO = 2

DEFAULT_SERVER_JS = "tools/rendezvous-server/rendezvous-server.js"
DEFAULT_CLIENT_C = "src/netplay/direct_p2p.c"


def find_one(path, pattern, what, flags=re.MULTILINE):
    """Return the single integer `pattern` captures in `path`, and its line.

    Requiring exactly one match is deliberate: zero means the definition moved
    or was renamed, several means the regex is no longer specific enough. Both
    make the comparison meaningless, so both are hard errors rather than a
    guess at which match was intended. This is not theoretical: the first
    draft of the host-floor pattern matched BOTH the REGISTER interval clamp
    and the STUN keepalive clamp, which have identical shape, and refusing was
    the correct outcome.
    """
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        print(f"[key-rate-budget] FAIL: cannot read {path}: {exc}", file=sys.stderr)
        return None, None

    matches = [
        (m.group(1), text.count("\n", 0, m.end(1)) + 1)
        for m in re.finditer(pattern, text, flags)
    ]
    if len(matches) != 1:
        print(
            f"[key-rate-budget] FAIL: expected exactly one definition of {what} "
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
    ap.add_argument("--server-js", default=None)
    ap.add_argument("--client-c", default=None)
    args = ap.parse_args()

    root = pathlib.Path(
        args.repo_root or pathlib.Path(__file__).resolve().parents[2]
    )
    server_js = (
        pathlib.Path(args.server_js) if args.server_js else root / DEFAULT_SERVER_JS
    )
    client_c = (
        pathlib.Path(args.client_c) if args.client_c else root / DEFAULT_CLIENT_C
    )

    # --- CLIENT side -----------------------------------------------------
    # The joiner's in-race REGISTER resend. A bare literal in the send gate,
    # not a #define, so the gate expression itself is the anchor.
    race_cadence_ms, race_line = find_one(
        client_c,
        r"\(\s*now\s*-\s*signal_last_send\s*\)\s*>=\s*(\d+)u",
        "the in-race REGISTER resend cadence",
    )
    # The host re-REGISTER worker's floor.
    #
    # Anchored on the Config_GetInt call that names the REGISTER interval key,
    # NOT on the clamp alone: `if (x < N) x = N;` on a variable called
    # `interval_ms` is a SHAPE, and direct_p2p.c has a second one (the STUN
    # keepalive's 5000 ms floor). Matching the key first makes the anchor
    # unique; the bounded `.{0,300}?` keeps it honest, so a clamp that drifts
    # out of the worker's setup block fails loudly instead of silently binding
    # to whatever came next.
    #
    # The backreference is load-bearing too: the comparison and the clamp must
    # be the SAME number, and a form where they differ is not the floor this
    # derivation assumes.
    host_floor_ms, host_line = find_one(
        client_c,
        r"Config_GetInt\(\s*CFG_KEY_NETPLAY_DIRECT_P2P_REGISTER_INTERVAL_MS\s*\)"
        r".{0,300}?if\s*\(\s*interval_ms\s*<\s*(\d+)\s*\)\s*interval_ms\s*=\s*\1\s*;",
        "the host re-REGISTER interval floor",
        flags=re.S,
    )

    # --- SERVER side -----------------------------------------------------
    key_window_ms, key_window_line = find_one(
        server_js,
        r"^const\s+KEY_RATE_WINDOW_MS\s*=\s*(\d+)\s*;",
        "KEY_RATE_WINDOW_MS",
    )
    key_cap, key_cap_line = find_one(
        server_js,
        r"^const\s+KEY_RATE_LIMIT_PER_WINDOW\s*=\s*(\d+)\s*;",
        "KEY_RATE_LIMIT_PER_WINDOW",
    )
    ip_cap, ip_cap_line = find_one(
        server_js,
        r"^const\s+RATE_LIMIT_PER_WINDOW\s*=\s*(\d+)\s*;",
        "RATE_LIMIT_PER_WINDOW",
    )
    dialers, dialers_line = find_one(
        server_js,
        r"^const\s+KEY_RATE_DESIGN_DIALERS\s*=\s*(\d+)\s*;",
        "KEY_RATE_DESIGN_DIALERS",
    )

    if None in (race_cadence_ms, host_floor_ms, key_window_ms, key_cap, ip_cap,
                dialers):
        return 2
    if race_cadence_ms <= 0 or host_floor_ms <= 0:
        print(
            f"[key-rate-budget] FAIL: a client cadence read as "
            f"{race_cadence_ms}/{host_floor_ms} ms; a zero or negative cadence "
            f"is an unbounded send rate and no cap can cover it.",
            file=sys.stderr,
        )
        return 1

    print(f"[key-rate-budget] client in-race REGISTER cadence={race_cadence_ms} ms "
          f"({client_c}:{race_line})")
    print(f"[key-rate-budget] client host re-REGISTER floor={host_floor_ms} ms "
          f"({client_c}:{host_line})")
    print(f"[key-rate-budget] server KEY_RATE_WINDOW_MS={key_window_ms} "
          f"({server_js}:{key_window_line})")
    print(f"[key-rate-budget] server KEY_RATE_LIMIT_PER_WINDOW={key_cap} "
          f"({server_js}:{key_cap_line})")
    print(f"[key-rate-budget] server RATE_LIMIT_PER_WINDOW={ip_cap} "
          f"({server_js}:{ip_cap_line})")
    print(f"[key-rate-budget] server KEY_RATE_DESIGN_DIALERS={dialers} "
          f"({server_js}:{dialers_line})")

    # The whole derivation is per-second arithmetic. If the window stops being
    # a second, every number below silently changes meaning, so this is a
    # "cannot compare" error rather than a violation.
    if key_window_ms != REQUIRED_WINDOW_MS:
        print(
            f"[key-rate-budget] FAIL: KEY_RATE_WINDOW_MS is {key_window_ms} ms, "
            f"not {REQUIRED_WINDOW_MS}. rendezvous-server.js's derivation and "
            f"this check are both written in requests-per-SECOND and are only "
            f"valid while per-window == per-second. Re-derive both, do not "
            f"relax this.",
            file=sys.stderr,
        )
        return 2

    host_per_sec = REQUIRED_WINDOW_MS / host_floor_ms
    joiner_per_sec = REQUIRED_WINDOW_MS / race_cadence_ms
    legit_peak = host_per_sec + dialers * joiner_per_sec

    # Constraint 2: absorption. One saturating cookied IP must not be able to
    # break a legitimate full room, so the cap must cover the legitimate peak
    # ON TOP OF a per-IP budget already spent by an attacker.
    absorption_floor = ip_cap + legit_peak
    # Constraint 1: an INTEGER multiple of the per-IP cap, at least
    # MIN_KEY_TO_IP_RATIO. Solve for the smallest k that satisfies both, which
    # is what the derivation means by "the smallest integer k satisfying that".
    k = max(MIN_KEY_TO_IP_RATIO, math.ceil(absorption_floor / ip_cap))
    required = k * ip_cap

    print(f"[key-rate-budget] legit peak = host {host_per_sec:g}/s + "
          f"{dialers} x {joiner_per_sec:g}/s = {legit_peak:g}/s")
    print(f"[key-rate-budget] required cap = k x per-IP with k = "
          f"max({MIN_KEY_TO_IP_RATIO}, ceil({absorption_floor:g}/{ip_cap})) "
          f"= {k}, so {k} x {ip_cap} = {required:g}/s; "
          f"deployed cap = {key_cap}/s")

    if key_cap < required:
        print(
            f"[key-rate-budget] FAIL: the deployed per-key cap of {key_cap}/s "
            f"is below the {required:g}/s the CLIENT's own cadences now "
            f"require. A full {dialers}-dialer room peaks at {legit_peak:g}/s "
            f"legitimately; with one cookied IP saturating its {ip_cap}/s the "
            f"room needs {absorption_floor:g}/s to keep pairing, so the "
            f"smallest INTEGER multiple of the {ip_cap}/s per-IP cap that "
            f"covers it is k = {k}, i.e. {required:g}/s.\n"
            f"[key-rate-budget] The server DEPLOYS SEPARATELY from the client, "
            f"so this is not a build break -- it is a room whose host liveness "
            f"REGISTERs get rate-dropped in production until SLOT_STALE_MS "
            f"reclaims a code that is still on the host's screen.\n"
            f"[key-rate-budget] Either revert the client cadence change, or "
            f"raise KEY_RATE_LIMIT_PER_WINDOW *and redeploy the server before "
            f"the client ships*.",
            file=sys.stderr,
        )
        return 1

    print("[key-rate-budget] PASS: the deployed per-key cap still covers the "
          "client's cadences.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
