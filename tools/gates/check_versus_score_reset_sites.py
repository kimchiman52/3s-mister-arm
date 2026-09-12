#!/usr/bin/env python3
"""Check that the win tally's three VersusScore_Reset sites are still where
the pairing boundaries are.

WHY THIS IS A SOURCE CHECK AND NOT A UNIT TEST. VersusScore_Reset
(src/hud/versus_score.c) is the pairing boundary of the versus win tally:
tallies to zero, names cleared, pending edge dropped. Its lifetime rules --
"starts at 0 with a new opponent", "survives a rematch and a trip through
character select", "resets only when the opponent changes" -- are not in
the function; they are in WHERE it is called (docs/versus-score.md):

  1. netplay.c -> Netplay_TickDirectP2P, the sole writer of
     NETPLAY_SESSION_TRANSITIONING, after setup_vs_mode() and before that
     store. (Netplay_BeginDirectP2P only arms the tick.)
  2. netplay.c -> Netplay_Run, the EXITING arm, before the sole store of
     NETPLAY_SESSION_IDLE -- every exit, disconnect and desync lands there.
  3. menu.c -> Mode_Select, the VERSUS case, after the sole store of
     `Mode_Type = MODE_VERSUS`.

No harness can drive Netplay_Run's EXITING arm, the deferred handoff tick
or the menu task, so deleting any of the three leaves every unit test
green -- test_netplay_units.c pins the core and the engine-bound lifetime,
not the placement. And a FOURTH call, added mid-set (say in VS_Result's
char-select branch), would silently break "survives char select", which
the doc says is true by construction. Same shape, and the same remedy, as
check_constant_time_compare.py: the property is the position of a call,
so it is checked at the source.

What is asserted:

  A. netplay.c stores NETPLAY_SESSION_TRANSITIONING exactly once, inside
     Netplay_TickDirectP2P, and that function calls VersusScore_Reset()
     exactly once, after setup_vs_mode() and before the store. The order
     is the rule docs/versus-score.md gives a future lobby for SetName.
  B. netplay.c stores NETPLAY_SESSION_IDLE exactly once, inside the EXITING
     arm of Netplay_Run, and that arm calls VersusScore_Reset() before it.
  C. menu.c stores `Mode_Type = MODE_VERSUS` exactly once (a commented-out
     copy does not count), inside Mode_Select, and VersusScore_Reset() is
     called between that store and the case's `break;`.
  D. Across src/**/*.c, excluding the test TUs and versus_score.c itself,
     `VersusScore_Reset();` appears exactly three times, all in those two
     files. A new boundary is a lifetime decision: change the doc and this
     check in the same commit.

Comments are stripped before matching, so a prose mention of a call is
not a site.

Exit codes: 0 = the shape holds, 1 = it is violated, 2 = a function, a
store or a file could not be located (which is itself a failure -- a check
that silently finds nothing is worse than no check).
"""

import argparse
import pathlib
import re
import sys

TAG = "[versus-score-reset-sites]"

RESET_CALL = re.compile(r"\bVersusScore_Reset\s*\(\s*\)\s*;")


def strip_comments(text):
    """Drop /* */ blocks and // line comments so prose cannot count as code.
    Trailing `//` is only treated as a comment when preceded by whitespace,
    which keeps a `://` inside a string literal intact."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"(?m)^\s*//[^\n]*", "", text)
    text = re.sub(r"(?m)\s//[^\n]*", "", text)
    return text


def extract_function(text, signature_re):
    """Return the body of the function whose DEFINITION matches signature_re,
    by brace matching from its opening brace; None if not found.

    The definition, not a prototype: signature_re is extended to demand a
    `{` after the parameter list with no `;` in between, because menu.c
    declares Mode_Select before defining it and a match on the prototype
    would brace-match its way into whatever function follows -- observed,
    the first run of this check failed on the real tree that way."""
    m = re.search(signature_re + r"[^;{]*\)\s*\{", text, re.M)
    if m is None:
        return None
    start = text.index("{", m.start())
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
    return None


def store_re(state):
    return re.compile(r"(?m)^\s*session_state\s*=\s*" + state + r"\s*;")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--netplay", default="src/netplay/netplay.c")
    ap.add_argument("--menu", default="src/sf33rd/Source/Game/menu/menu.c")
    ap.add_argument("--src-root", default="src")
    args = ap.parse_args()

    netplay_path = pathlib.Path(args.netplay)
    menu_path = pathlib.Path(args.menu)
    src_root = pathlib.Path(args.src_root)
    for p in (netplay_path, menu_path):
        if not p.is_file():
            print(f"{TAG} ERROR: {p} does not exist", file=sys.stderr)
            return 2
    if not src_root.is_dir():
        print(f"{TAG} ERROR: {src_root} is not a directory", file=sys.stderr)
        return 2

    netplay = strip_comments(netplay_path.read_text())
    menu = strip_comments(menu_path.read_text())
    failures = []
    errors = []

    # --- A. Netplay_TickDirectP2P: setup_vs_mode -> Reset -> TRANSITIONING --
    trans = store_re("NETPLAY_SESSION_TRANSITIONING")
    n_trans = len(trans.findall(netplay))
    if n_trans != 1:
        failures.append(
            f"{netplay_path} stores NETPLAY_SESSION_TRANSITIONING {n_trans} "
            f"time(s), not 1 -- the session-start boundary is no longer a "
            f"single point, so the tally's start reset no longer covers "
            f"every session")
    tick = extract_function(netplay, r"^void\s+Netplay_TickDirectP2P\s*\(")
    if tick is None:
        errors.append(
            f"could not locate Netplay_TickDirectP2P in {netplay_path} -- "
            f"the session-start boundary was renamed or moved")
    else:
        resets = list(RESET_CALL.finditer(tick))
        setup = re.search(r"\bsetup_vs_mode\s*\(\s*\)\s*;", tick)
        store = trans.search(tick)
        if len(resets) != 1:
            failures.append(
                f"Netplay_TickDirectP2P calls VersusScore_Reset() "
                f"{len(resets)} time(s), not once -- the netplay start "
                f"boundary is unpinned")
        if store is None:
            failures.append(
                "Netplay_TickDirectP2P no longer stores "
                "NETPLAY_SESSION_TRANSITIONING -- the start boundary moved "
                "and the reset did not move with it")
        if setup is None:
            errors.append(
                "Netplay_TickDirectP2P no longer calls setup_vs_mode() -- "
                "the ordering rule this check pins has lost its anchor")
        if resets and store is not None and setup is not None:
            r = resets[0].start()
            if not (setup.start() < r < store.start()):
                failures.append(
                    "in Netplay_TickDirectP2P, VersusScore_Reset() must sit "
                    "AFTER setup_vs_mode() and BEFORE the "
                    "NETPLAY_SESSION_TRANSITIONING store -- that order is "
                    "the SetName rule docs/versus-score.md gives a future "
                    "lobby (names set before the reset are lost)")

    # --- B. Netplay_Run, EXITING arm: Reset before the sole IDLE store ------
    idle = store_re("NETPLAY_SESSION_IDLE")
    n_idle = len(idle.findall(netplay))
    if n_idle != 1:
        failures.append(
            f"{netplay_path} stores NETPLAY_SESSION_IDLE {n_idle} time(s), "
            f"not 1 -- a session can now end without passing the tally's "
            f"end reset")
    run = extract_function(netplay, r"^void\s+Netplay_Run\s*\(")
    if run is None:
        errors.append(
            f"could not locate Netplay_Run in {netplay_path}")
    else:
        arms = [m.start() for m in re.finditer(
            r"case\s+NETPLAY_SESSION_EXITING\s*:", run)]
        if len(arms) != 1:
            errors.append(
                f"Netplay_Run has {len(arms)} `case NETPLAY_SESSION_EXITING:` "
                f"label(s), expected exactly 1")
        else:
            nxt = re.search(r"case\s+NETPLAY_SESSION_", run[arms[0] + 1:])
            end = arms[0] + 1 + nxt.start() if nxt else len(run)
            arm = run[arms[0]:end]
            reset = RESET_CALL.search(arm)
            store = idle.search(arm)
            if store is None:
                failures.append(
                    "Netplay_Run's EXITING arm no longer stores "
                    "NETPLAY_SESSION_IDLE -- the teardown moved and this "
                    "check is looking at the wrong arm")
            if reset is None:
                failures.append(
                    "Netplay_Run's EXITING arm no longer calls "
                    "VersusScore_Reset() -- the tally survives the opponent "
                    "leaving, and the next session starts with the old set's "
                    "numbers until its own reset")
            elif store is not None and not (reset.start() < store.start()):
                failures.append(
                    "in Netplay_Run's EXITING arm, VersusScore_Reset() must "
                    "precede the NETPLAY_SESSION_IDLE store")

    # --- C. menu.c Mode_Select, VERSUS case: the sole MODE_VERSUS store ------
    versus_store = re.compile(r"(?m)^\s*Mode_Type\s*=\s*MODE_VERSUS\s*;")
    n_versus = len(versus_store.findall(menu))
    if n_versus != 1:
        failures.append(
            f"{menu_path} stores `Mode_Type = MODE_VERSUS` {n_versus} "
            f"time(s), not 1 -- local versus no longer has a single entry, "
            f"and the entry is the only honest boundary the doc could find")
    mode_select = extract_function(menu, r"^void\s+Mode_Select\s*\(")
    if mode_select is None:
        errors.append(f"could not locate Mode_Select in {menu_path}")
    else:
        store = versus_store.search(mode_select)
        if store is None:
            failures.append(
                "Mode_Select no longer stores `Mode_Type = MODE_VERSUS` -- "
                "the local-versus entry moved and the reset did not move "
                "with it")
        else:
            brk = re.search(r"\bbreak\s*;", mode_select[store.end():])
            case_tail = mode_select[store.end():
                                    store.end() + (brk.start() if brk else 0)]
            if RESET_CALL.search(case_tail) is None:
                failures.append(
                    "the VERSUS case in Mode_Select no longer calls "
                    "VersusScore_Reset() between the MODE_VERSUS store and "
                    "its break -- a second local set starts with the first "
                    "set's numbers")

    # --- D. Exactly three sites tree-wide, all in those two files ------------
    sites = []
    for p in sorted(src_root.rglob("*.c")):
        if p.name.startswith("test_") or p.name == "versus_score.c":
            continue
        for _ in RESET_CALL.finditer(strip_comments(p.read_text())):
            sites.append(p)
    n_sites = len(sites)
    if n_sites != 3:
        where = ", ".join(str(p) for p in sites) or "none"
        failures.append(
            f"VersusScore_Reset() is called {n_sites} time(s) under "
            f"{src_root} ({where}), not 3 -- a new pairing boundary is a "
            f"lifetime decision (a reset mid-set breaks 'survives character "
            f"select'); change docs/versus-score.md and this check in the "
            f"same commit")
    allowed = {netplay_path.resolve(), menu_path.resolve()}
    stray = sorted({str(p) for p in sites if p.resolve() not in allowed})
    if stray:
        failures.append(
            f"VersusScore_Reset() is called outside the two boundary files: "
            f"{', '.join(stray)}")

    if errors:
        print(f"{TAG} ERROR: this check could not find what it guards; it "
              f"now guards nothing.", file=sys.stderr)
        for e in errors:
            print(f"{TAG}   - {e}", file=sys.stderr)
        for f in failures:
            print(f"{TAG}   - (also) {f}", file=sys.stderr)
        return 2
    if failures:
        print(f"{TAG} FAIL: the win tally's reset sites no longer match "
              f"docs/versus-score.md.", file=sys.stderr)
        for f in failures:
            print(f"{TAG}   - {f}", file=sys.stderr)
        print(f"{TAG} No unit test can catch this: the EXITING arm, the "
              f"deferred handoff tick and the menu task need a live session "
              f"or the menu task, so test_netplay_units.c stays green while "
              f"the lifetime rules regress.", file=sys.stderr)
        return 1

    print(f"{TAG} PASS: VersusScore_Reset() at its three sites -- "
          f"Netplay_TickDirectP2P (after setup_vs_mode, before the sole "
          f"TRANSITIONING store), Netplay_Run's EXITING arm (before the sole "
          f"IDLE store), Mode_Select's VERSUS case (after the sole "
          f"MODE_VERSUS store) -- and nowhere else.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
